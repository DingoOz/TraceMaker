#!/usr/bin/env python3
"""Shows violations of one type that only KiCad or only TraceMaker reports, matched by item positions.

Usage: drc_detail.py board.kicad_pcb type [limit]   (run drc_parity.py first to fill the caches)
"""
import hashlib, json, pathlib, subprocess, sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
f = pathlib.Path(sys.argv[1]); ty = sys.argv[2]; lim = int(sys.argv[3]) if len(sys.argv) > 3 else 8
st = f.stat(); k = hashlib.sha1(f"{f.resolve()}:{st.st_mtime_ns}:{st.st_size}".encode()).hexdigest()[:16]
kd = json.load(open(ROOT / f"build/drc/kicad/{k}.json"))
out = ROOT / "build/drc/detail.json"
subprocess.run([str(ROOT / "build/release/src/app/tracemaker"), "drc", str(f), "--json", str(out)], capture_output=True)
td = json.load(open(out))
def items(d):
    vs = d["unconnected_items"] if ty == "unconnected_items" else [v for v in d["violations"] if v["type"] == ty]
    return vs
def keyset(v):
    return frozenset((round(i["pos"]["x"], 2), round(i["pos"]["y"], 2)) for i in v["items"])
K, T = items(kd), items(td)
kk = {keyset(v): v for v in K}; tk = {keyset(v): v for v in T}
def near(a, pool):  # any item position within 0.05 mm of an item of a pool violation
    for b in pool:
        if any(abs(x[0] - y[0]) < 0.06 and abs(x[1] - y[1]) < 0.06 for x in a for y in b):
            return True
    return False
only_k = [v for s, v in kk.items() if not near(s, tk)]
only_t = [v for s, v in tk.items() if not near(s, kk)]
print(f"{ty}: kicad {len(K)}, ours {len(T)}; only kicad {len(only_k)}, only ours {len(only_t)}")
for v in only_k[:lim]:
    print("  K", v.get("description", "")[:110], "|", [(i["description"][:45], i["pos"]["x"], i["pos"]["y"]) for i in v["items"]])
for v in only_t[:lim]:
    print("  T", v.get("actual_mm"), v.get("required_mm"), "|", [(i["description"][:45], i["pos"]["x"], i["pos"]["y"]) for i in v["items"]])
