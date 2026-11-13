# Oracle for placement advice (doc 17 stage 4): turn the K movable parts with the most pads by 180 degrees in a copy of the board and write
# advice that puts them back. Usage: scramble.py board.kicad_pcb out.kicad_pcb advice.json K
import json, re, sys
src, out, adv, k = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
t = open(src).read()
# footprint blocks: find "(module " or "(footprint " at depth 1, then match parentheses
blocks = []
for m in re.finditer(r'\n  \((module|footprint) ', t):
    i = m.start() + 3; d = 0; j = i
    while True:
        c = t[j]
        if c == '(': d += 1
        elif c == ')':
            d -= 1
            if d == 0: break
        elif c == '"':
            j += 1
            while t[j] != '"': j += 2 if t[j] == '\\' else 1
        j += 1
    blocks.append((i, j + 1))
cands = []
for (i, j) in blocks:
    b = t[i:j]
    head = b[:300]
    if re.search(r'\(locked|\blocked\b', head): continue
    ref = re.search(r'fp_text reference "?([^"\s)]+)"?|property "Reference" "([^"]+)"', b)
    if not ref: continue
    ref = ref.group(1) or ref.group(2)
    if ref.startswith(('J', 'P', 'SW', 'H', 'MH', 'REF', 'X', 'CN', 'CON', 'TP')): continue  # connectors and holes are fixed by the placer anyway
    at = re.search(r'\(at ([-\d.]+) ([-\d.]+)(?: ([-\d.]+))?\)', b)
    pads = b.count('(pad ')
    cands.append((-pads, ref, i, at))
cands.sort()
chosen = cands[:k]
moves = []
edits = []
for _, ref, i, at in chosen:
    x, y, r = float(at.group(1)), float(at.group(2)), float(at.group(3) or 0)
    nr = (r + 180) % 360
    edits.append((i + at.start(), i + at.end(), '(at %g %g %g)' % (x, y, nr)))
    moves.append({"ref": ref, "at": [x, y], "rotate": r})
for s, e, new in sorted(edits, reverse=True): t = t[:s] + new + t[e:]
open(out, 'w').write(t)
json.dump({"version": 1, "note": "oracle: the designer's orientation of the parts turned by 180 degrees",
           "seeds": [{"name": "designer orientation", "moves": moves}],
           "eco": moves}, open(adv, 'w'), indent=1)
print(' '.join(m['ref'] for m in moves))
