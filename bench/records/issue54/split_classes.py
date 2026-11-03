#!/usr/bin/env python3
"""Put every net of a KiCad project in a net class of its own, with the values of the class it had (issue #54).

usage: split_classes.py BOARD.kicad_pcb   (rewrites the .kicad_pro beside it)
"""
import fnmatch
import json
import pathlib
import re
import sys

pcb = pathlib.Path(sys.argv[1])
text = pcb.read_text()
nets = sorted((set(re.findall(r'\(net \d+ "([^"]+)"\)', text)) | set(re.findall(r'\(net "([^"]+)"\)', text))) - {""})
pro = pcb.with_suffix(".kicad_pro")
d = json.loads(pro.read_text())
ns = d["net_settings"]
old = dict(ns.get("netclass_assignments") or {})
patterns = ns.get("netclass_patterns") or []
by_name = {c["name"]: c for c in ns["classes"]}


def class_of(net):
    if net in old:
        return old[net] if isinstance(old[net], str) else old[net][0]
    for p in patterns:
        if fnmatch.fnmatchcase(net, p["pattern"]):
            return p["netclass"]
    return "Default"


assign = {}
for i, n in enumerate(nets):
    c = dict(by_name[class_of(n)])
    c["name"], c["priority"] = f"C{i}", i
    ns["classes"].append(c)
    assign[n] = [f"C{i}"]
ns["netclass_assignments"] = assign
ns["netclass_patterns"] = []
pro.write_text(json.dumps(d, indent=2))
print(f"{len(nets)} nets; {len(ns['classes'])} classes")
