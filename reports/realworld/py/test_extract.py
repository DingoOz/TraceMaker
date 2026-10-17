import sys, pathlib, time
sys.path.insert(0, str(pathlib.Path(__file__).parent))
import extract, kgeom
b = kgeom.load("/home/dingo/Programming/TraceMaker/reports/realworld/boards/kitspace_teensy-fx/human/routed.kicad_pcb")
ids = {n: i for i, n in b.nets.items()}
nl = extract.Netlist()
t0 = time.time()
sel = ["BCLK", "LRCLK", "I2S_IN", "I2S_OUT", "MCLK", "Net-(C14-Pad1)", "Net-(C15-Pad1)"]
for n in sel:
    extract.build_net(b, ids[n], nl, 4)
pairs = extract.couple(b, nl, {ids[n] for n in sel})
extract.emit_caps(nl)
print("elements", len(nl.lines), "time", round(time.time() - t0, 1), "s")
for n in sel:
    print(n, nl.stats[n], [k for k in nl.pin_nodes if k[2] == n])
for k, v in pairs.items():
    print([b.nets[x] for x in k], {a: round(c, 4) for a, c in v.items()})
