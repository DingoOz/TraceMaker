#!/usr/bin/env python3
"""Hand-overs of the legaliser probe: per log, each ramp stage and the first variant's legalise + warm start after it, and the final count."""
import re, sys, glob, os
for f in sorted(glob.glob(sys.argv[1] + '/*.log')):
    name = os.path.basename(f)[:-4]
    lines = open(f).read().splitlines()
    out, stage = [], None
    for i, l in enumerate(lines):
        m = re.match(r'ramp stage (\d)/\d .*: (\d+) of (\d+) routed', l)
        if m: stage = f"s{m.group(1)}:{m.group(2)}"; seen = False; continue
        m = re.match(r'legalize: .*?(\d+) pairs too close before, (\d+) after', l)
        if m and stage and not seen:
            w = next((x for x in lines[i+1:i+3] if x.startswith('warm start')), '')
            k = re.search(r'(\d+) saved routes taken over, (\d+) not legal', w)
            out.append(f"{stage} pairs {m.group(1)}->{m.group(2)} kept {k.group(1) if k else '?'}")
            seen = True
    fin = [l for l in lines if l.startswith('routed ')]
    print(f"{name:40s} {' | '.join(out)}  FINAL {fin[-1].split(',')[0] if fin else '-'}")
