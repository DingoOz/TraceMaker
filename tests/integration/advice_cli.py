#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Advice end to end (doc 17, issue #26) through the command line:

  advice_cli.py <tracemaker> <workdir>

- the advisor is not consulted when everything routes; it is when connections are left, gets a stuck report on stdin, and its advice is
  saved beside the output with the board's real names (also when the report was redacted)
- an advisor that fails, is slow, or answers rubbish leaves the run as it was: output written, exit code unchanged, the reason in the summary
- the saved advice repeats the run: the same board bytes twice, and at 1, 2 and 4 threads (work budget)
- bad advice files are refused with the file named (exit 1); a good one with unknown names says which
- routing without any advice option gives the same bytes as before the option existed (a run is a run)

Uses no fixtures and no KiCad: the board is generated.
"""
import hashlib
import json
import pathlib
import subprocess
import sys


def problem():
    obstacles, connections = [], []

    def pad(i, part, x, y):
        obstacles.append({"type": "rect", "layers": ["top"], "center": {"x": x, "y": y}, "width": 0.6, "height": 0.5, "connectedTo": [i],
                          "obstacleId": "pad_" + i, "componentId": part})

    def conn(name, a, ax, ay, b, bx, by):
        connections.append({"name": name, "pointsToConnect": [{"x": ax, "y": ay, "layer": "top", "pointId": a}, {"x": bx, "y": by, "layer": "top", "pointId": b}],
                            "rootConnectionName": name})

    pad("a1", "U1", -8, 0), pad("a2", "U2", 8, 0), pad("b1", "U3", 0, -6), pad("b2", "U4", 0, 6), pad("c1", "U5", -8, 8), pad("c2", "U6", -4, 8)
    conn("NET_A", "a1", -8, 0, "a2", 8, 0)
    conn("NET_B", "b1", 0, -6, "b2", 0, 6)
    conn("NET_C", "c1", -8, 8, "c2", -4, 8)
    return {"layerCount": 2, "minTraceWidth": 0.12, "bounds": {"minX": -10, "maxX": 10, "minY": -10, "maxY": 10}, "obstacles": obstacles, "connections": connections}


def sha(p):
    return hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()


def main():
    tm, work = sys.argv[1], pathlib.Path(sys.argv[2])
    work.mkdir(parents=True, exist_ok=True)
    (work / "p.json").write_text(json.dumps(problem()))
    board = work / "b.kicad_pcb"
    subprocess.run([tm, "srj-import", str(work / "p.json"), "-o", str(board)], check=True, capture_output=True)

    def route(name, *args, check=True, stdin=None):
        out = work / f"{name}.kicad_pcb"
        cmd = [tm, "route", str(board), "-o", str(out), "--no-kb", "--json", str(work / f"{name}.json")] + list(args)
        p = subprocess.run(cmd, capture_output=True, text=True, input=stdin)
        if check and p.returncode not in (0, 3):
            raise SystemExit(f"{name}: exit {p.returncode}\n{p.stderr}\n{p.stdout}")
        js = work / f"{name}.json"
        return p, out, (json.loads(js.read_text()) if js.exists() else None)

    ok = True

    def check(cond, msg):
        nonlocal ok
        if not cond:
            print("FAIL:", msg)
            ok = False

    # A mock advisor: records that it ran and what it was sent, answers with a fixed advice file.
    mock = work / "mock.py"
    seen = work / "seen.json"
    mock.write_text(f"""import json, sys
rep = json.load(sys.stdin)
open({str(seen)!r}, 'w').write(json.dumps(rep))
names = [u['net'] for u in rep['unrouted']]
json.dump({{"version": 1, "note": "mock", "first_nets": names[:1], "variants": 2}}, sys.stdout)
""")
    work_args = ["--work", "300000", "--variants", "2", "--threads", "2"]

    # 1. Everything routes: the advisor is never started.
    seen.unlink(missing_ok=True)
    _, _, j = route("complete", *work_args, "--advisor", f"{sys.executable} {mock}")
    check(j["routed"] == j["connections"], "the generated board should route completely")
    check(not seen.exists(), "the advisor was consulted although everything was routed")
    check(not (work / "complete.advice.json").exists(), "advice was saved although none was asked for")

    # 2. Connections left (no budget): consulted, report on stdin, advice saved with real names, summary says what happened.
    stuck = ["--work", "1", "--variants", "2", "--threads", "2"]
    p, out, j = route("stuck", *stuck, "--advisor", f"{sys.executable} {mock}", "--stuck-report", str(work / "stuck.report.json"))
    check(seen.exists(), "the advisor was not consulted although connections were left")
    rep = json.loads(seen.read_text())
    check(rep["tracemaker_stuck_report"] == 1 and rep["unrouted"], "no stuck report on stdin")
    check(rep["disclosure"]["redacted"] is False and "advice_schema" in rep, "the report lacks its disclosure or schema")
    check(json.loads((work / "stuck.report.json").read_text())["result"]["unrouted"] == rep["result"]["unrouted"], "--stuck-report differs from stdin")
    saved = work / "stuck.advice.json"
    check(saved.exists(), "the advice was not saved beside the output")
    adv = json.loads(saved.read_text())
    check(adv["version"] == 1 and adv["first_nets"][0].startswith("NET_"), f"saved advice has wrong content: {adv}")
    rounds = j["advice"]["advisor"]["rounds"]
    check(len(rounds) == 1 and rounds[0]["advisor"]["ok"] is True and "adopted" in rounds[0], f"summary lacks the advisor round: {rounds}")
    check("sh -c" not in json.dumps(j) and rounds[0]["advisor"]["program"], "summary should name the program only")
    check(out.exists(), "no output board")

    # 3. Redacted: the advisor sees aliases, the saved advice has the board's names.
    seen.unlink(missing_ok=True)
    mock2 = work / "mock2.py"
    mock2.write_text(f"""import json, sys
rep = json.load(sys.stdin)
open({str(seen)!r}, 'w').write(json.dumps(rep))
json.dump({{"version": 1, "first_nets": [rep['unrouted'][0]['net']]}}, sys.stdout)
""")
    _, _, j = route("redacted", *stuck, "--advisor", f"{sys.executable} {mock2}", "--advisor-redact")
    text = seen.read_text()
    check("NET_" not in text and "U1" not in text and json.loads(text)["disclosure"]["redacted"] is True, "the redacted report leaks the board's names")
    adv = json.loads((work / "redacted.advice.json").read_text())
    check(adv["first_nets"][0].startswith("NET_"), f"the saved advice should have the board's names, got {adv['first_nets']}")

    # 4. A broken advisor changes nothing: output written, exit code as without it, the reason in the summary.
    base_p, base_out, base_j = route("noadvisor", *stuck)
    for label, cmd, needle in [("fail", "exit 3", "status 3"), ("rubbish", "echo hello", "refused"), ("unknown", "echo '{\"version\":1,\"x\":1}'", "unknown key"),
                               ("names", "echo '{\"version\":1,\"first_nets\":[\"NOPE\"]}'", "no net named"), ("missing", "no_such_advisor_xyz 2>/dev/null", "status 127")]:
        p, out, j = route("adv_" + label, *stuck, "--advisor", cmd)
        check(p.returncode == base_p.returncode, f"{label}: exit code {p.returncode} differs from the run without an advisor ({base_p.returncode})")
        check(sha(out) == sha(base_out), f"{label}: the output differs from the run without an advisor")
        err = j["advice"]["advisor"]["rounds"][0].get("error", "")
        check(needle in err, f"{label}: the summary should say '{needle}', got '{err}'")
    p, out, j = route("adv_slow", *stuck, "--advisor", "sleep 20", "--advisor-timeout", "0.5")
    check(sha(out) == sha(base_out) and "did not answer" in j["advice"]["advisor"]["rounds"][0].get("error", ""), "a slow advisor should be a warning only")

    # 5. The saved advice repeats a run, at any thread count.
    sa = str(work / "stuck.advice.json")
    shas = set()
    for threads in ("1", "2", "4"):
        _, out, _ = route("rep" + threads, "--work", "400000", "--variants", "2", "--threads", threads, "--advice", sa)
        shas.add(sha(out))
    check(len(shas) == 1, "the same advice file gave different boards at 1, 2 and 4 threads")
    _, out_a, _ = route("again", "--work", "400000", "--variants", "2", "--threads", "2", "--advice", sa)
    check(sha(out_a) == next(iter(shas)), "the same advice file gave a different board the second time")
    _, out_plain1, _ = route("plain1", "--work", "400000", "--variants", "2", "--threads", "2")
    _, out_plain2, _ = route("plain2", "--work", "400000", "--variants", "2", "--threads", "4")
    check(sha(out_plain1) == sha(out_plain2), "without advice the thread count changed the board")

    # 6. Bad advice is refused with the file named, before any routing.
    bad = work / "bad.json"
    for label, text, needle in [("key", '{"version": 1, "frist_nets": []}', "unknown key"), ("net", '{"version": 1, "first_nets": ["NET_Z"]}', "no net named"),
                                ("syntax", '{"version": 1,\n "first_nets": [,]}', "line 2"), ("layer", '{"version": 1, "layers": {"no_tracks": ["F.Cu", "B.Cu"]}}', "every copper layer")]:
        bad.write_text(text)
        p = subprocess.run([tm, "route", str(board), "-o", str(work / "bad.kicad_pcb"), "--advice", str(bad), "--work", "1000"], capture_output=True, text=True)
        check(p.returncode == 1 and str(bad) in p.stderr and needle in p.stderr, f"{label}: bad advice not refused as expected (exit {p.returncode}): {p.stderr[-300:]}")
        c = subprocess.run([tm, "advice-check", str(board), str(bad)], capture_output=True, text=True)
        check(c.returncode == 1 and needle in c.stderr, f"{label}: advice-check should refuse it too")
    good = work / "good.json"
    good.write_text(json.dumps({"version": 1, "first_nets": ["NET_B"], "layers": {"cost": {"F.Cu": 1.5}},
                                "regions": [{"rect": [-3, -2, 3, 2], "reserve_for": ["NET_A"]}]}))
    c = subprocess.run([tm, "advice-check", str(board), str(good)], capture_output=True, text=True)
    check(c.returncode == 0 and "reserved for NET_A" in c.stdout, f"advice-check on a good file: {c.stdout} {c.stderr}")
    _, out_g, jg = route("good", "--work", "400000", "--variants", "2", "--threads", "2", "--advice", str(good))
    check(jg["routed"] == jg["connections"] and jg["advice"]["advised_best_routed"] >= 0, "advice with a region should still route the board")
    check(any(v["variant"] >= 1000 for v in jg["variants"]), "no advised variant in the summary")

    print("advice_cli: " + ("ok" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
