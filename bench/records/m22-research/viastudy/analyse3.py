#!/usr/bin/env python3
"""One cause per open connection, first match: routes alone (A, FB or W at 2 M) / more search (BIG, CHEAP, FINE) / foreign vias within 0.5, 1, 2 mm /
foreign tracks within 2 mm / both within 2 mm / none. Uses out/<board>.json and out/<board>.fb.json."""
import json, pathlib, sys
from collections import Counter
OUT = pathlib.Path(__file__).parent / "out"
cols = ["alone", "search", "via0.5", "via1", "via2", "tracks2", "both2", "none"]
tot = Counter()
print("| board | sampled | " + " | ".join(cols) + " | via-rescues also rescued by tracks-only |")
print("|---|--:|" + "--:|" * len(cols) + "--:|")
for b in sys.argv[1:]:
    d = json.loads((OUT / f"{b}.json").read_text())
    fbp = OUT / f"{b}.fb.json"
    fb = {}
    if fbp.exists():
        for x in json.loads(fbp.read_text())["connections"]:
            fb[(x["net"], x["a"], x["b"])] = x["exp"]
    c = Counter()
    for x in d["connections"]:
        e = dict(x["exp"]); e.update({k: v for k, v in fb.get((x["net"], x["a"], x["b"]), {}).items() if k in ("FB", "W")})
        r = lambda k: e.get(k, {}).get("routed", False)
        c["n"] += 1
        if r("A") or r("FB") or r("W"): k = "alone"
        elif r("BIG") or r("CHEAP") or r("FINE"): k = "search"
        elif r("V05"): k = "via0.5"
        elif r("V1"): k = "via1"
        elif r("V2"): k = "via2"
        elif r("T2"): k = "tracks2"
        elif r("VT2"): k = "both2"
        else: k = "none"
        c[k] += 1
        if k.startswith("via") and r("T2"): c["vt"] += 1
    tot.update(c)
    print(f"| {b} | {c['n']} | " + " | ".join(str(c[k]) for k in cols) + f" | {c['vt']} |")
print(f"| total | {tot['n']} | " + " | ".join(str(tot[k]) for k in cols) + f" | {tot['vt']} |")
