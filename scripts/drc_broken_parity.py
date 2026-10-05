#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DRC parity on broken boards: injects defects into routed boards and compares how TraceMaker's DRC and
kicad-cli classify them (shorts between nets, crossings, dangling tracks and vias, missing connections).

Usage: drc_broken_parity.py [--kicad-jobs 2] [--jobs 4] [--count 3] [--seed 1] [--kinds k1,k2] [--json out]
                            board.kicad_pcb ...
For every base board and defect kind, `tracemaker selftest-defects` writes a variant (with the base's project
and rules files) to build/scratch/broken/<board>/<kind>.kicad_pcb; both DRCs then run on it. Violations are
matched one to one by type and item positions (0.06 mm), so a disagreement in classification (e.g. a short
reported as a clearance violation) shows as one unmatched violation on each side. KiCad results are cached
in build/drc/kicad/ like drc_parity.py. Exit status 1 when any variant disagrees.
"""
import argparse
import concurrent.futures as cf
import hashlib
import json
import pathlib
import re
import shutil
import subprocess
import sys
from collections import Counter, defaultdict

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from drc_parity import CACHE, ROOT, TM, TYPES  # noqa: E402

KINDS = ["stub", "stub_via", "via_free", "cut", "shorten", "short_cross", "short_touch", "short_via", "short_pad",
         "short_netless", "short_zone", "cross_same"]
SCRATCH = ROOT / "build" / "scratch" / "broken"
TOL = 0.06  # mm: item positions closer than this are the same item


def content_key(path: pathlib.Path) -> str:
    """Cache key from the board and its rules files' contents (variants are rewritten on every run)."""
    h = hashlib.sha1()
    for f in (path, path.with_suffix(".kicad_pro"), path.with_suffix(".kicad_dru")):
        h.update(f.read_bytes() if f.exists() else b"-")
    return "c" + h.hexdigest()[:16]


def kicad_report(path: pathlib.Path, timeout: int) -> dict | None:
    CACHE.mkdir(parents=True, exist_ok=True)
    out = CACHE / f"{content_key(path)}.json"
    if not out.exists():
        try:
            subprocess.run(["kicad-cli", "pcb", "drc", "--format", "json", "--severity-all", "--all-track-errors",
                            "-o", str(out), str(path)], capture_output=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            return None
    return json.loads(out.read_text()) if out.exists() else None


def tm_report(path: pathlib.Path, timeout: int, tm: pathlib.Path = TM, out: pathlib.Path | None = None) -> dict | None:
    out = out or path.with_suffix(".tm.json")
    out.unlink(missing_ok=True)
    try:
        subprocess.run([str(tm), "drc", str(path), "--json", str(out)], capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None
    return json.loads(out.read_text()) if out.exists() else None


def net_of(v: dict) -> str:
    """Net named in the first item's description ("Pad 1 of U1 [GND]", "PTH pad 1 [GND] of U1")."""
    m = re.search(r"\[(.*)\]", v["items"][0]["description"]) if v.get("items") else None
    if not m:
        return ""
    name = m.group(1)  # KiCad shows net names unescaped; TraceMaker keeps the file's escapes
    for esc, ch in (("{slash}", "/"), ("{backslash}", "\\"), ("{lt}", "<"), ("{gt}", ">"), ("{colon}", ":"),
                    ("{dblquote}", '"'), ("{tab}", "\t"), ("{return}", "\n")):
        name = name.replace(esc, ch)
    return name


# Documented exceptions (docs/03-data-structures.md §6): TraceMaker's DRC does not check copper text, so KiCad's
# violations that involve a text item are counted separately instead of as mismatches.
TEXT_ITEM = re.compile(r"\btext\b", re.IGNORECASE)


def is_exception(v: dict) -> bool:
    return any(TEXT_ITEM.search(i.get("description", "")) for i in v.get("items", []))


def violations(d: dict, excluded: Counter | None = None) -> list[tuple]:
    """(type, item positions, description, net) for every routing-relevant violation, unconnected items included."""
    out = []
    for v in d.get("violations", []):
        if v["type"] in TYPES:
            if is_exception(v):
                if excluded is not None:
                    excluded[v["type"]] += 1
                continue
            out.append((v["type"], [(i["pos"]["x"], i["pos"]["y"]) for i in v["items"]], v.get("description", ""), net_of(v)))
    for v in d.get("unconnected_items", []):
        out.append(("unconnected_items", [(i["pos"]["x"], i["pos"]["y"]) for i in v["items"]], v.get("description", ""),
                    net_of(v)))
    return out


def near(p, q, tol: float = TOL) -> bool:
    return abs(p[0] - q[0]) < tol and abs(p[1] - q[1]) < tol


def same_items(a, b, strict: bool, tol: float = TOL) -> bool:
    """strict: the same number of items, pairwise near (in some order); else one list's items all near the other's
    (KiCad lists a board edge or zone as a second item where TraceMaker may list one)."""
    if strict:
        return len(a) == len(b) and (all(near(p, q, tol) for p, q in zip(a, b)) or
                                     (len(a) == 2 and near(a[0], b[1], tol) and near(a[1], b[0], tol)))
    return all(any(near(p, q) for q in b) for p in a) or all(any(near(p, q) for q in a) for p in b)


def match(kv, tv):
    """One-to-one matching by type and item positions, strict pairs first; returns (matched, only_kicad, only_ours).
    Unconnected items are matched by net, then by count (both tools pick their own ratsnest end points)."""
    used_t = [False] * len(tv)
    used_k = [False] * len(kv)
    # Exact positions first (tracks shorter than the tolerance would otherwise pair up crosswise), then near.
    for strict, tol in ((True, 0.001), (True, TOL), (False, TOL)):
        for i, k in enumerate(kv):
            if used_k[i]:
                continue
            for j, t in enumerate(tv):
                if used_t[j] or t[0] != k[0]:
                    continue
                # Unconnected items: same net first; then by count (KiCad may name an item of another net found at
                # a ratsnest end point, e.g. a zone under a via).
                if (k[3] == t[3] or not strict) if k[0] == "unconnected_items" else same_items(k[1], t[1], strict, tol):
                    used_t[j] = used_k[i] = True
                    break
    matched = [k for i, k in enumerate(kv) if used_k[i]]
    only_k = [k for i, k in enumerate(kv) if not used_k[i]]
    only_t = [t for j, t in enumerate(tv) if not used_t[j]]
    return matched, only_k, only_t


def subtract(found, base):
    """`found` without one occurrence of each violation in `base` (same type and items)."""
    rest = list(found)
    for strict in (True, False):
        for b in list(base):
            for i, f in enumerate(rest):
                if f[0] == b[0] and ((f[3] == b[3] or not strict) if f[0] == "unconnected_items" else same_items(f[1], b[1], strict)):
                    del rest[i]
                    base = [x for x in base if x is not b]
                    break
    return rest


def variant_dir(base: pathlib.Path) -> pathlib.Path:
    return SCRATCH / f"{base.parent.name}__{base.stem}".replace(" ", "_")  # PCBench boards share file names


def make_variant(base: pathlib.Path, kind: str, seed: int, count: int) -> tuple[pathlib.Path, int]:
    d = variant_dir(base)
    d.mkdir(parents=True, exist_ok=True)
    out = d / f"{kind}.kicad_pcb"
    for ext in (".kicad_pro", ".kicad_dru"):  # both DRCs read the rules from the project next to the board
        src = base.with_suffix(ext)
        if src.exists():
            shutil.copyfile(src, out.with_suffix(ext))
    man = out.with_suffix(".defects.json")
    subprocess.run([str(TM), "selftest-defects", str(base), str(out), "--kind", kind, "--seed", str(seed),
                    "--count", str(count), "--manifest", str(man)], check=True, capture_output=True)
    return out, len(json.loads(man.read_text())["defects"])


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("boards", nargs="*")
    ap.add_argument("--list", help="file with one base board per line (# comments)")
    ap.add_argument("--tm", default=str(TM), help="tracemaker binary whose DRC is compared (default: this build)")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--kicad-jobs", type=int, default=2)
    ap.add_argument("--timeout", type=int, default=600)
    ap.add_argument("--count", type=int, default=3)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--kinds", default=",".join(KINDS))
    ap.add_argument("--json", help="write per-variant results here")
    ap.add_argument("-v", "--verbose", action="store_true", help="list the unmatched violations")
    ap.add_argument("--base-delta", action="store_true",
                    help="discount the mismatches the unmodified board already has (for bases that do not match exactly)")
    a = ap.parse_args()
    kinds = a.kinds.split(",")
    names = list(a.boards)
    if a.list:
        names += [ln.strip() for ln in open(a.list) if ln.strip() and not ln.startswith("#")]
    bases = [pathlib.Path(b) for b in names if pathlib.Path(b).is_file()]
    if len(bases) != len(names):
        print("missing boards:", [b for b in names if not pathlib.Path(b).is_file()])
        return 77
    jobs = [(b, k) for b in bases for k in kinds]
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        made = list(ex.map(lambda j: make_variant(j[0], j[1], a.seed, a.count), jobs))
    paths = [p for p, n in made if n > 0]
    with cf.ThreadPoolExecutor(a.kicad_jobs) as ex:
        kr = dict(zip(paths, ex.map(lambda p: kicad_report(p, a.timeout), paths)))
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        tr = dict(zip(paths, ex.map(lambda p: tm_report(p, a.timeout, pathlib.Path(a.tm)), paths)))

    base_diff = {}
    if a.base_delta:
        with cf.ThreadPoolExecutor(a.kicad_jobs) as ex:
            kb = dict(zip(bases, ex.map(lambda b: kicad_report(b, a.timeout), bases)))
        with cf.ThreadPoolExecutor(a.jobs) as ex:
            tb = dict(zip(bases, ex.map(lambda b: tm_report(b, a.timeout, pathlib.Path(a.tm), variant_dir(b) / "base.tm.json"), bases)))
        for b in bases:
            if kb[b] is not None and tb[b] is not None:
                _, bk, bt = match(violations(kb[b], Counter()), violations(tb[b]))
                base_diff[b] = (bk, bt)

    per_type = defaultdict(Counter)  # type -> matched / only_kicad / only_ours
    per_kind = defaultdict(Counter)  # kind -> variants / exact
    rows, exact, total = [], 0, 0
    excluded = Counter()  # KiCad violations involving copper text (documented exception)
    for (base, kind), (path, n) in zip(jobs, made):
        if n == 0:
            continue
        k, t = kr.get(path), tr.get(path)
        if k is None or t is None:
            print(f"ERR    {base.parent.name}/{kind}")
            continue
        m, ok, ot = match(violations(k, excluded), violations(t))
        if base in base_diff:
            ok, ot = subtract(ok, base_diff[base][0]), subtract(ot, base_diff[base][1])
        total += 1
        per_kind[kind]["variants"] += 1
        for v in m:
            per_type[v[0]]["matched"] += 1
        for v in ok:
            per_type[v[0]]["only_kicad"] += 1
        for v in ot:
            per_type[v[0]]["only_ours"] += 1
        good = not ok and not ot
        exact += good
        per_kind[kind]["exact"] += good
        diff = Counter(v[0] for v in ok)
        diff.subtract(Counter(v[0] for v in ot))
        detail = " ".join(f"{ty}:K+{sum(1 for v in ok if v[0] == ty)}/T+{sum(1 for v in ot if v[0] == ty)}"
                          for ty in sorted({v[0] for v in ok + ot}))
        print(f"{'MATCH' if good else 'DIFF':6} {(base.parent.name if base.stem in ("processed", "raw", "unrouted") else base.stem)[:28]:28} {kind:14} {n} defects  {detail}")
        if a.verbose:
            for v in ok:
                print("    K", v[0], v[2][:70], v[1])
            for v in ot:
                print("    T", v[0], v[2][:70], v[1])
        rows.append({"board": str(base), "kind": kind, "defects": n, "variant": str(path),
                     "only_kicad": [list(v[:2]) for v in ok], "only_ours": [list(v[:2]) for v in ot]})
    print(f"\n{exact}/{total} broken variants classified exactly like KiCad")
    if excluded:
        print("excluded (KiCad violations with copper text, not checked by TraceMaker):",
              " ".join(f"{ty}={n}" for ty, n in sorted(excluded.items())))
    print(f"{'type':22} {'matched':>8} {'only KiCad':>11} {'only ours':>10}")
    for ty in TYPES:
        c = per_type.get(ty)
        if c:
            print(f"{ty:22} {c['matched']:8} {c['only_kicad']:11} {c['only_ours']:10}")
    print(f"\n{'defect kind':16} exact/variants")
    for kd in kinds:
        c = per_kind.get(kd)
        if c:
            print(f"{kd:16} {c['exact']}/{c['variants']}")
    if a.json:
        pathlib.Path(a.json).write_text(json.dumps({"exact": exact, "variants": total, "per_type": per_type,
                                                    "per_kind": per_kind, "excluded": excluded, "rows": rows}, indent=1))
    return 0 if exact == total else 1


if __name__ == "__main__":
    sys.exit(main())
