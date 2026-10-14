#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The footprint check against datasheet land patterns through the command line (issue #5, doc 05 section 43):

  footprint_check.py <tracemaker> <workdir>

- `footprint-check` on a board with a footprint that is the datasheet's pattern, one with a pad moved and one with no reference: the text report, the JSON, the exit status
  (4 on a discrepancy), --only, --require, --all, --strict, --trust-all, the trusted brand list, errors for a missing or malformed library, the same report twice
- `route --footprint-refs`: a footprint that matches changes nothing (the routed board is byte-identical), one that does not stops the job before it routes, and
  --footprint-warn-only routes anyway and puts the findings in the summary
"""
import hashlib
import json
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]


def sha(p):
    return hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()


def soic8(move_pad3=0.0):
    pads = []
    for i in range(4):
        pads.append((str(i + 1), -2.7, -1.905 + 1.27 * i + (move_pad3 if i == 2 else 0.0), 1.55, 0.6))
    for i in range(4):
        pads.append((str(i + 5), 2.7, 1.905 - 1.27 * i, 1.55, 0.6))
    return pads


def board_text(parts):
    out = ['(kicad_pcb (version 20240108) (generator "test") (layers (0 "F.Cu" signal) (31 "B.Cu" signal) (44 "Edge.Cuts" user))']
    for ref, lib, value, x, y, pads, props in parts:
        out.append(f'(footprint "{lib}" (layer "F.Cu") (at {x} {y} 0) (property "Reference" "{ref}") (property "Value" "{value}")')
        for k, v in props:
            out.append(f' (property "{k}" "{v}")')
        for n, px, py, w, h in pads:
            out.append(f' (pad "{n}" smd rect (at {px} {py} 0) (size {w} {h}) (layers "F.Cu" "F.Mask"))')
        out.append(')')
    out.append(')')
    return "\n".join(out) + "\n"


def refs_json(brand, pads=None, tolerance=None):
    pads = pads or soic8()
    entry = {
        "brand": brand, "part": "EX1234", "package": "SOIC-8",
        "datasheet": {"document": "EX1234 datasheet", "revision": "C", "page": 21},
        "match": {"mpn": ["EX1234*"]},
        "pads": [{"number": n, "at": [x, y], "size": [w, h]} for n, x, y, w, h in pads],
    }
    if tolerance is not None:
        entry["tolerance_mm"] = {"position": tolerance}
    return json.dumps({"format": "tracemaker-footprint-refs/1", "references": [entry]}, indent=1)


def main():
    tm, work = sys.argv[1], pathlib.Path(sys.argv[2])
    shutil.rmtree(work, ignore_errors=True)  # no output of an earlier run may stand in for this one
    work.mkdir(parents=True, exist_ok=True)
    ok = True

    def check(cond, msg):
        nonlocal ok
        if not cond:
            print("FAIL:", msg)
            ok = False

    def run(*args):
        return subprocess.run([tm, *args], capture_output=True, text=True)

    props = [("MPN", "EX1234-TR")]
    board = work / "b.kicad_pcb"
    board.write_text(board_text([
        ("U1", "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm", "EX1234", 20, 20, soic8(), props),
        ("U2", "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm", "EX1234", 40, 20, soic8(move_pad3=0.15), props),
        ("R1", "Resistor_SMD:R_0603_1608Metric", "10k", 60, 20, [("1", -0.825, 0, 0.8, 0.95), ("2", 0.825, 0, 0.8, 0.95)], []),
    ]))
    refs = work / "refs"
    refs.mkdir(exist_ok=True)
    (refs / "ex.json").write_text(refs_json("Example Semiconductor"))
    (refs / "trusted_brands.txt").write_text("# datasheets we believe\nExample Semiconductor | Example Semi\n")

    # a discrepancy: exit 4, named in the text and the JSON
    r = run("footprint-check", str(board), "--refs", str(refs), "--json", str(work / "a.json"))
    check(r.returncode == 4, f"a footprint that differs from its datasheet should exit 4, got {r.returncode}: {r.stderr[-300:]}")
    check("U2" in r.stdout and "MISMATCH vs Example Semiconductor EX1234" in r.stdout and "pad 3" in r.stdout, f"the report should name U2, the datasheet and pad 3: {r.stdout[:400]}")
    check(not any(line.startswith("U1 ") for line in r.stdout.splitlines()), "U1 is fine and is not listed without --all")
    j = json.loads((work / "a.json").read_text())
    by = {f["ref"]: f for f in j["footprints"]}
    check(j["summary"]["checked"] == 3 and j["summary"]["ok"] == 1 and j["summary"]["mismatch"] == 1 and j["summary"]["unverified"] == 1, f"summary: {j['summary']}")
    check(by["U1"]["status"] == "ok" and by["U2"]["status"] == "mismatch" and by["R1"]["status"] == "unverified", f"statuses: {[(k, v['status']) for k, v in by.items()]}")
    check(by["U2"]["issues"][0]["kind"] == "position" and by["U2"]["issues"][0]["pad"] == "3" and abs(by["U2"]["issues"][0]["actual_mm"] - 0.15) < 0.002, f"U2 issues: {by['U2']['issues']}")
    check(by["U2"]["reference"]["page"] == 21 and by["U2"]["reference"]["brand"] == "Example Semiconductor", f"the datasheet should be named: {by['U2'].get('reference')}")
    r2 = run("footprint-check", str(board), "--refs", str(refs), "--json", str(work / "b.json"))
    check((work / "a.json").read_text() == (work / "b.json").read_text(), "the same board and library gave a different report")

    # selection and strictness
    check(run("footprint-check", str(board), "--refs", str(refs), "--only", "U1").returncode == 0, "--only U1: nothing to call out")
    all_out = run("footprint-check", str(board), "--refs", str(refs), "--all", "--only", "U1,R1").stdout
    check("U1" in all_out and "ok vs" in all_out and "R1" in all_out and "no reference" in all_out, f"--all lists the fine and the unreferenced ones: {all_out}")
    req = run("footprint-check", str(board), "--refs", str(refs), "--only", "R1", "--require", "R*", "--json", str(work / "req.json"))
    r1 = [f for f in json.loads((work / "req.json").read_text())["footprints"] if f["ref"] == "R1"][0]
    check(req.returncode == 4 and r1["issues"][0]["kind"] == "no_reference", f"--require R*: R1 has no reference, an error: {req.returncode}")

    # a brand nobody trusts gives no verdict; --strict makes the warning count; --trust-all uses it, and says so
    other = work / "refs_other"
    other.mkdir(exist_ok=True)
    (other / "ex.json").write_text(refs_json("Some Random Site"))
    (other / "trusted_brands.txt").write_text("Example Semiconductor\n")
    u = run("footprint-check", str(board), "--refs", str(other), "--only", "U2")
    check(u.returncode == 0 and "UNTRUSTED" in u.stdout, f"an untrusted reference is a warning, not a verdict: {u.returncode} {u.stdout[:300]}")
    check(run("footprint-check", str(board), "--refs", str(other), "--only", "U2", "--strict").returncode == 4, "--strict: an untrusted reference counts")
    ta = run("footprint-check", str(board), "--refs", str(other), "--only", "U2", "--trust-all")
    check(ta.returncode == 4 and "trust-all" in ta.stdout, f"--trust-all uses the reference and the report says so: {ta.returncode} {ta.stdout[:300]}")
    (other / "trusted_brands.txt").unlink()
    none = run("footprint-check", str(board), "--refs", str(other), "--only", "U2")
    check("no trusted brand list" in none.stdout, f"without a list the report says what to do: {none.stdout[:300]}")
    # a tolerance in the reference is the datasheet's own
    (refs / "ex.json").write_text(refs_json("Example Semiconductor", tolerance=0.2))
    check(run("footprint-check", str(board), "--refs", str(refs)).returncode == 0, "a reference with a 0.2 mm position tolerance accepts 0.15 mm")
    (refs / "ex.json").write_text(refs_json("Example Semiconductor"))
    check(run("footprint-check", str(board), "--refs", str(refs), "--position-tol-mm", "0.2").returncode == 0, "--position-tol-mm 0.2 accepts 0.15 mm")

    # refusals
    m = run("footprint-check", str(board), "--refs", str(work / "nothing"))
    check(m.returncode == 1 and "no such reference file or directory" in m.stderr, f"a missing library is an error: {m.returncode} {m.stderr[-200:]}")
    (refs / "bad.json").write_text('{"format": "tracemaker-footprint-refs/1", "references": [{"brand": "X", "part": "Y", "tolerence_mm": {}}]}')
    b = run("footprint-check", str(board), "--refs", str(refs))
    check(b.returncode == 1 and "unknown key 'tolerence_mm'" in b.stderr, f"a misspelt key is refused: {b.returncode} {b.stderr[-200:]}")
    (refs / "bad.json").unlink()

    # route: the check runs first
    srj = {"layerCount": 2, "minTraceWidth": 0.12, "bounds": {"minX": -10, "maxX": 10, "minY": -10, "maxY": 10},
           "obstacles": [{"type": "rect", "layers": ["top"], "center": {"x": x, "y": y}, "width": 0.6, "height": 0.5, "connectedTo": [i], "obstacleId": "pad_" + i, "componentId": part}
                         for i, part, x, y in [("a1", "U1", -8, 0), ("a2", "U2", 8, 0)]],
           "connections": [{"name": "NET_A", "pointsToConnect": [{"x": -8, "y": 0, "layer": "top", "pointId": "a1"}, {"x": 8, "y": 0, "layer": "top", "pointId": "a2"}], "rootConnectionName": "NET_A"}]}
    (work / "p.json").write_text(json.dumps(srj))
    rb = work / "r.kicad_pcb"
    subprocess.run([tm, "srj-import", str(work / "p.json"), "-o", str(rb)], check=True, capture_output=True)
    pad_ref = {"format": "tracemaker-footprint-refs/1", "references": [{"brand": "Example Semiconductor", "part": "SRJ-U", "datasheet": {"document": "SRJ"}, "match": {"ref": ["U1", "U2"]},
                                                                      "pads": [{"number": "1", "at": [0, 0], "size": [0.6, 0.5]}]}]}
    good = work / "refs_route_good"
    good.mkdir(exist_ok=True)
    (good / "r.json").write_text(json.dumps(pad_ref))
    (good / "trusted_brands.txt").write_text("Example Semiconductor\n")
    bad = work / "refs_route_bad"
    bad.mkdir(exist_ok=True)
    pad_ref["references"][0]["pads"][0]["size"] = [0.9, 0.5]  # the datasheet says 0.9 mm wide
    (bad / "r.json").write_text(json.dumps(pad_ref))
    (bad / "trusted_brands.txt").write_text("Example Semiconductor\n")
    base = ["--work", "300000", "--variants", "2", "--threads", "2", "--no-kb"]
    plain = run("route", str(rb), "-o", str(work / "plain.kicad_pcb"), "--json", str(work / "plain.json"), *base)
    g = run("route", str(rb), "-o", str(work / "good.kicad_pcb"), "--json", str(work / "good.json"), "--footprint-refs", str(good), *base)
    check(g.returncode == plain.returncode and sha(work / "plain.kicad_pcb") == sha(work / "good.kicad_pcb"), "--footprint-refs with nothing wrong must not change the routed board")
    check("footprint check: 2 footprints against 1 datasheet references" in g.stdout, f"the log should say what was checked: {g.stdout[:400]}")
    gj = json.loads((work / "good.json").read_text())
    check(gj.get("footprint_check", {}).get("summary", {}).get("ok") == 2, f"the summary should say both are ok: {gj.get('footprint_check')}")
    check("footprint_check" not in json.loads((work / "plain.json").read_text()), "no check unless asked")
    e = run("route", str(rb), "-o", str(work / "stopped.kicad_pcb"), "--footprint-refs", str(bad), *base)
    check(e.returncode == 1 and "footprint check: 2 errors" in e.stderr, f"a footprint that differs stops the job: {e.returncode} {e.stderr[-300:]}")
    check(not (work / "stopped.kicad_pcb").exists(), "nothing is routed or written when the check fails")
    w = run("route", str(rb), "-o", str(work / "warned.kicad_pcb"), "--json", str(work / "warned.json"), "--footprint-refs", str(bad), "--footprint-warn-only", *base)
    check(w.returncode == plain.returncode and sha(work / "plain.kicad_pcb") == sha(work / "warned.kicad_pcb"), f"--footprint-warn-only routes anyway: {w.returncode} {w.stderr[-200:]}")
    wj = json.loads((work / "warned.json").read_text())
    check(wj.get("footprint_check", {}).get("summary", {}).get("errors") == 2, f"the findings are in the summary: {wj.get('footprint_check', {}).get('summary')}")
    check(len(wj["footprint_check"]["footprints"]) == 2 and wj["footprint_check"]["footprints"][0]["issues"][0]["kind"] == "size", "the summary lists the footprints with issues")
    t = run("route", str(rb), "-o", str(work / "tol.kicad_pcb"), "--footprint-refs", str(bad), "--footprint-tol-mm", "0.4", *base)
    check(t.returncode == plain.returncode, f"--footprint-tol-mm 0.4 accepts a 0.3 mm size difference: {t.returncode}")

    print("footprint_check: " + ("ok" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
