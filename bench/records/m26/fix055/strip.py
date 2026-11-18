#!/usr/bin/env python3
"""strip.py in.kicad_pcb out_dir nets [via_x,via_y,ball_x,ball_y]: remove every track and via of the given nets
(comma separated); optionally add a fixed dog-bone (F.Cu stub + via) for the first net."""
import re, sys, shutil, pathlib, uuid
src, out, nets = sys.argv[1], pathlib.Path(sys.argv[2]), sys.argv[3].split(",")
t = open(src).read()
n0 = len(t)
def keep(m):
    return "" if re.search(r'\(net "([^"]*)"\)', m[0])[1] in nets else m[0]
t, k = re.subn(r'\t\((?:segment|via|arc)\n.*?\n\t\)\n', keep, t, flags=re.S)
if len(sys.argv) > 4:
    vx, vy, bx, by = map(float, sys.argv[4].split(","))
    u = lambda s: uuid.uuid5(uuid.NAMESPACE_URL, s)
    add = (f'\t(segment\n\t\t(start {bx} {by})\n\t\t(end {vx} {vy})\n\t\t(width 0.12)\n\t\t(layer "F.Cu")\n\t\t(net "{nets[0]}")\n\t\t(uuid "{u("s"+nets[0])}")\n\t)\n'
           f'\t(via\n\t\t(at {vx} {vy})\n\t\t(size 0.3)\n\t\t(drill 0.15)\n\t\t(layers "F.Cu" "B.Cu")\n\t\t(net "{nets[0]}")\n\t\t(uuid "{u("v"+nets[0])}")\n\t)\n')
    i = t.rstrip().rfind(")"); t = t[:i] + add + t[i:]
out.mkdir(parents=True, exist_ok=True)
open(out / "board.kicad_pcb", "w").write(t)
for ext in ("kicad_pro", "kicad_prl"):
    p = pathlib.Path(src).with_suffix("." + ext)
    if p.exists(): shutil.copy(p, out / f"board.{ext}")
print("removed", n0 - len(t) if len(sys.argv) <= 4 else "items", "chars")
