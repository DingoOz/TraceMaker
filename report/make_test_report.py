#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generates the data tables of report/roadmap_testing.tex from the project's own records.

  python3 report/make_test_report.py        # writes report/generated/*.tex
  build/tools/tectonic report/roadmap_testing.tex

Sources: dev/progress.json (roadmap), build/release/ctest-junit.xml (ctest --output-junit) and the test sources
(which file and tags each test has), tests/CMakeLists.txt (labels, time limits), bench/results/*/summary.json and
boards.jsonl (every benchmark run), bench/place_sets/H.json (held-out set).
Nothing here is typed in by hand: a number in a generated table is the number in the record.
"""
import json
import pathlib
import re
import xml.etree.ElementTree as ET

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "report/generated"

SYMBOLS = {"≥": r"$\geq$", "≤": r"$\leq$", "→": r"$\to$", "–": "--", "—": "---", "§": r"\S", "×": r"$\times$", "µ": r"$\mu$", "Ω": r"$\Omega$",
           "−": r"$-$", "“": "``", "”": "''", "’": "'", "±": r"$\pm$", "°": r"$^\circ$", "…": r"\ldots{}", "≈": r"$\approx$", "é": r"\'e"}


def esc(text) -> str:
    """Plain text -> LaTeX, with `code` spans as \\texttt."""
    s = "" if text is None else str(text)
    parts = s.split("`")
    out = []
    for i, part in enumerate(parts):
        p = part.replace("\\", r"\textbackslash{}")
        for a, b in (("&", r"\&"), ("%", r"\%"), ("$", r"\$"), ("#", r"\#"), ("_", r"\_"), ("{", r"\{"), ("}", r"\}"), ("~", r"\textasciitilde{}"),
                     ("^", r"\textasciicircum{}"), ("<", r"\textless{}"), (">", r"\textgreater{}"), ("|", r"\textbar{}")):
            p = p.replace(a, b)
        p = p.replace(r"\textbackslash\{\}", r"\textbackslash{}")
        for a, b in SYMBOLS.items():
            p = p.replace(a, b)
        p = p.replace('"', "''")
        out.append(r"\texttt{" + p.replace("--", r"-{}-") + "}" if i % 2 == 1 else p)
    return "".join(out)


def brk(name: str) -> str:
    """A long identifier that may break after _ - / ."""
    return esc(name).replace(r"\_", r"\_\allowbreak{}").replace("-", r"-\allowbreak{}").replace("/", r"/\allowbreak{}")


def roadmap() -> str:
    d = json.loads((ROOT / "dev/progress.json").read_text())
    out = []
    for m in d["milestones"]:
        gate, _, result = m["gate"].partition(" | ")
        out.append(r"\subsection*{" + esc(m["id"]) + ": " + esc(m["title"]) + "}")
        out.append(r"\addcontentsline{toc}{subsection}{" + esc(m["id"]) + ": " + esc(m["title"]) + "}")
        out.append(r"\noindent\textbf{Deliverables.} " + esc(m["deliverables"]) + r"\par\smallskip")
        out.append(r"\noindent\textbf{Gate.} " + esc(gate) + r"\par\smallskip")
        if result:
            out.append(r"\noindent\colorbox{boxfill}{\parbox{\dimexpr\linewidth-2\fboxsep}{\textbf{Gate result.} " + esc(result) + r"}}\par\smallskip")
        done = sum(t["done"] for t in m["tasks"])
        out.append(r"\noindent\textbf{Tasks} (" + f"{done} of {len(m['tasks'])} closed; status: {esc(m['status'])}" + r").")
        out.append(r"\begin{itemize}[leftmargin=1.6em,itemsep=1pt]")
        for t in m["tasks"]:
            mark = r"\item[\textcolor{fraqua}{$\checkmark$}]" if t["done"] else r"\item[\textcolor{frorange}{$\circ$}]"
            out.append(mark + " " + esc(t["text"]))
        out.append(r"\end{itemize}")
    return "\n".join(out) + "\n"


def roadmap_summary() -> str:
    d = json.loads((ROOT / "dev/progress.json").read_text())
    rows = []
    for m in d["milestones"]:
        _, _, result = m["gate"].partition(" | ")
        built = sum(1 for t in m["tasks"] if t["done"] and "closed, not built" not in t["text"])
        closed = sum(1 for t in m["tasks"] if "closed, not built" in t["text"])
        verdict = "met" if not result else ("not met" if "NOT met" in result else "not measured" if "NOT measured" in result or "not measured" in result else "see text")
        rows.append(f"{esc(m['id'])} & {esc(m['title'])} & {len(m['tasks'])} & {built} & {closed} & {verdict} \\\\")
    return "\n".join(rows) + "\n"


def test_sources() -> dict:
    """Catch2 test name -> (file, tags)."""
    out = {}
    files = list((ROOT / "tests").glob("*.cpp")) + list((ROOT / "src").glob("*/test_*.cpp"))
    for f in files:
        for m in re.finditer(r'TEST_CASE\(\s*"((?:[^"\\]|\\.)*)"\s*(?:,\s*"([^"]*)")?', f.read_text()):
            out[m.group(1).replace('\\"', '"')] = (str(f.relative_to(ROOT)), m.group(2) or "")
    return out


def ctest_meta() -> dict:
    """add_test name -> (labels, timeout, what it checks)."""
    text = (ROOT / "tests/CMakeLists.txt").read_text()
    for extra in (ROOT / "src").glob("*/CMakeLists.txt"):
        text += "\n" + extra.read_text()
    text += "\n" + (ROOT / "bindings/CMakeLists.txt").read_text() if (ROOT / "bindings/CMakeLists.txt").exists() else ""
    meta = {}
    for m in re.finditer(r"set_tests_properties\((\w+) PROPERTIES ([^\n]*)\)", text):
        labels = re.search(r'LABELS "([^"]*)"', m.group(2))
        timeout = re.search(r"TIMEOUT (\d+)", m.group(2))
        meta[m.group(1)] = (labels.group(1) if labels else "", timeout.group(1) if timeout else "")
    return meta


INTEGRATION = {
    "kicad_edit_roundtrip": "Boards edited by TraceMaker (tracks and vias added, footprints moved and rotated) are read back by pcbnew in Docker; geometry must equal TraceMaker's own reading.",
    "kicad_drc_parity": "TraceMaker's DRC against `kicad-cli pcb drc` on 14 KiCad demo boards: the same violations, type by type (15 routing-relevant types).",
    "kicad_drc_broken_parity": "264 boards with injected defects (11 demo boards, 12 defect kinds, 2 seeds): TraceMaker's DRC must match KiCad violation by violation.",
    "drc_zone_index": "DRC reports through the zone-fill edge index are byte-identical to the linear reference path (`drc --linear-zones`) on four zone boards.",
    "global_route": "`route --global` and `--global-congestion` are repeatable byte for byte at a fixed work budget and stay within 5 connections of routing without them.",
    "record_replay": "A board routed plainly, with `--record` and with `--view` is byte-identical; replaying the recorded log reproduces the routed board.",
    "python_bindings": "The Python module reads, routes, checks and emits items; `route` gives the same bytes as the CLI.",
    "pcm_package": "Builds the KiCad PCM archive and validates its layout and metadata (also with kicad-python's validator).",
    "length_tuning": "A net with a custom `length` rule is meandered into range; KiCad's DRC judges the result.",
    "crules_catalogue_sync": "The committed rule catalogue JSON equals `docs/component_rules.yaml` and the rule ids used in doc 15.",
    "crules_route": "Routing with `--component-rules on`: no new track enters a generated keep-out, the keep-outs are not written into the board, the sidecar `.kicad_dru` is written.",
    "crules_dru_parses": "KiCad itself parses the generated sidecar `.kicad_dru` without errors.",
    "crules_two_stage": "Two-stage placement with component rules: the result is legal, and parts locked after stage 1 are where the decoupling-only placement put them.",
    "reach_verify": "Every \"unreachable\" verdict of the flood-fill pre-check is re-checked by the full A*: zero mismatches.",
    "thread_determinism": "The same `--work` portfolio at 1, 3 and 8 threads (and with fewer variants) writes identical bytes.",
    "diff_pair": "A USB hub's five differential pairs are routed coupled at the rule's gap, twice with identical bytes, and KiCad's DRC is clean.",
}


def tests() -> tuple[str, str]:
    junit = ROOT / "build/release/ctest-junit.xml"
    cases = []
    if junit.exists():
        for tc in ET.parse(junit).getroot().iter("testcase"):
            status = tc.get("status", "run")
            result = "skipped" if tc.find("skipped") is not None or status in ("notrun", "disabled") else "FAILED" if tc.find("failure") is not None or status == "fail" else "passed"
            cases.append((tc.get("name"), float(tc.get("time") or 0), result))
    src = test_sources()
    meta = ctest_meta()
    groups: dict[str, list] = {}
    for name, t, result in cases:
        if name in src:
            f, tags = src[name]
        else:
            f, tags = "integration (" + "tests/CMakeLists.txt" + ")", meta.get(name, ("", ""))[0]
        groups.setdefault(f, []).append((name, tags, t, result))
    lines = []
    summary = []
    order = sorted(groups, key=lambda f: (f.startswith("integration"), f))
    for f in order:
        rows = groups[f]
        total = sum(r[2] for r in rows)
        summary.append(f"{brk(f)} & {len(rows)} & {sum(r[3] == 'passed' for r in rows)} & {sum(r[3] == 'skipped' for r in rows)} & {sum(r[3] == 'FAILED' for r in rows)} & {total:.2f} \\\\")
        lines.append(r"\subsection*{" + brk(f) + f" ({len(rows)} tests, {total:.1f} s)" + "}")
        lines.append(r"\begin{longtable}{@{}p{0.57\linewidth}p{0.19\linewidth}rl@{}}")
        lines.append(r"\toprule Test & Tags / labels & Time (s) & Result \\ \midrule \endhead")
        for name, tags, t, result in rows:
            what = ""
            if name in INTEGRATION:
                what = r"\newline{\footnotesize\color{ink2} " + esc(INTEGRATION[name]) + (f" Time limit {meta[name][1]} s." if name in meta and meta[name][1] else "") + "}"
            lines.append(f"{brk(name) if ' ' not in name else esc(name)}{what} & {{\\footnotesize {esc(tags)}}} & {t:.2f} & {result} \\\\")
        lines.append(r"\bottomrule\end{longtable}")
    n = len(cases)
    head = (f"{n} tests: {sum(c[2] == 'passed' for c in cases)} passed, {sum(c[2] == 'skipped' for c in cases)} skipped, "
            f"{sum(c[2] == 'FAILED' for c in cases)} failed; {sum(c[1] for c in cases):.0f} s of test time")
    return "\n".join(lines) + "\n", "\n".join(summary) + "\n" + r"\midrule \multicolumn{6}{@{}l}{" + esc(head) + r"} \\" + "\n"


def pct(v) -> str:
    return "--" if v is None else f"{100 * v:.1f}"


def bench_runs() -> str:
    rows = []
    for f in sorted((ROOT / "bench/results").glob("*/summary.json"), key=lambda p: p.stat().st_mtime):
        try:
            s = json.loads(f.read_text())
        except ValueError:
            continue
        if "clean_pass" not in s:
            continue
        rows.append(f"{brk(s.get('run', f.parent.name))} & {{\\footnotesize {esc(s.get('set', ''))}}} & {s.get('boards', '')} & {pct(s.get('clean_pass'))} & "
                    f"{pct(s.get('completion'))} & {pct(s.get('fr_rc12_clean_pass'))} & {s.get('boards_with_added_errors', '')} & {esc(s.get('commit', ''))} \\\\")
    return "\n".join(rows) + "\n"


def boards(run: str) -> list[dict]:
    p = ROOT / "bench/results" / run / "boards.jsonl"
    return [json.loads(line) for line in p.read_text().splitlines()] if p.exists() else []


def tier_tables() -> str:
    out = []
    for tier in "ABCD":
        rows = boards(f"final11-tier{tier}")
        if not rows:
            continue
        clean = sum(bool(r.get("clean")) for r in rows)
        out.append(r"\subsection*{Tier " + tier + f": {clean} of {len(rows)} clean, {sum(r.get('routed', 0) for r in rows)} of {sum(r.get('connections', 0) for r in rows)} connections" + "}")
        out.append(r"\begin{longtable}{@{}p{0.36\linewidth}rrrrcp{0.22\linewidth}@{}}")
        out.append(r"\toprule Board & Conn. & Routed & Vias & Time (s) & Clean & Freerouting 2.5 \\ \midrule \endhead")
        for r in sorted(rows, key=lambda r: r["board"].lower()):
            fr = r.get("fr_rc12") or {}
            frs = "--" if not fr else ("clean" if fr.get("clean") else f"{fr.get('unrouted', '?')} open" + (f", {fr.get('violations')} viol." if fr.get("violations") else ""))
            mark = r"\textcolor{fraqua}{yes}" if r.get("clean") else r"\textcolor{frorange}{no}"
            added = sum((r.get("added_errors") or {}).values())
            out.append(f"{brk(r['board'])} & {r.get('connections', '')} & {r.get('routed', '')} & {r.get('vias', '')} & {r.get('seconds', 0):.0f} & {mark}"
                       + (f" ({added} err.)" if added else "") + f" & {{\\footnotesize {esc(frs)}}} \\\\")
        out.append(r"\bottomrule\end{longtable}")
    return "\n".join(out) + "\n"


def held_table() -> str:
    human = {r["board"]: r for r in boards("H-none-2")}
    scratch = {r["board"]: r for r in boards("S-routable-7")}
    info = {}
    hj = ROOT / "bench/place_sets/H.json"
    if hj.exists():
        info = {b["board"]: b for b in json.loads(hj.read_text())["boards"]}
    out = []
    for name in sorted(human, key=str.lower):
        h, s = human[name], scratch.get(name, {})
        i = info.get(name, {})

        def cell(r):
            if not r:
                return "--"
            mark = r"\textcolor{fraqua}{clean}" if r.get("clean") else r"\textcolor{frorange}{not clean}"
            extra = sum((r.get("place_added") or {}).values())
            return f"{r.get('routed', '?')}/{r.get('connections', '?')} {mark}" + (f" ({extra} pl.\\ err.)" if extra else "")
        out.append(f"{brk(name)} & {i.get('parts', '')} & {i.get('movable', '')} & {cell(h)} & {cell(s)} & {s.get('place_s', 0):.0f} \\\\")
    return "\n".join(out) + "\n"


def main() -> None:
    OUT.mkdir(exist_ok=True)
    (OUT / "roadmap.tex").write_text(roadmap())
    (OUT / "roadmap_summary.tex").write_text(roadmap_summary())
    body, summary = tests()
    (OUT / "tests.tex").write_text(body)
    (OUT / "tests_summary.tex").write_text(summary)
    (OUT / "bench_runs.tex").write_text(bench_runs())
    (OUT / "tiers.tex").write_text(tier_tables())
    (OUT / "held.tex").write_text(held_table())
    print("wrote", ", ".join(sorted(p.name for p in OUT.glob("*.tex"))))


if __name__ == "__main__":
    main()
