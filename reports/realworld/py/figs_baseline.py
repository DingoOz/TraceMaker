# SPDX-License-Identifier: GPL-3.0-or-later
import pathlib, sys
sys.path.insert(0, str(pathlib.Path(__file__).parent))
import draw, kgeom
import matplotlib.pyplot as plt

ROOT = pathlib.Path("/home/dingo/Programming/TraceMaker")
FIX = ROOT / "bench/data/freerouting/scripts/benchmark/fixtures/PCBench"
B = ROOT / "reports/realworld/boards"
OUT = ROOT / "reports/realworld/figs"
OUT.mkdir(exist_ok=True)
LABELS = {"kitspace_teensy-fx": ("U1", "U3", "U4", "U5", "U6", "J1", "J2", "RV1", "RV2"),
          "Hardware_Playground_serial_gw_ATMEGA328P": ("IC1", "U1", "U2", "U3", "U4", "P1", "CON1", "X1")}
SHORT = {"kitspace_teensy-fx": "teensy", "Hardware_Playground_serial_gw_ATMEGA328P": "serial"}

for name, short in SHORT.items():
    ref = kgeom.load(str(FIX / name / "unrouted.kicad_pcb"))
    for tag, board, unc in (
        ("place", ref, ()),
        ("base", kgeom.load(str(B / name / "T0-50000000/routed.kicad_pcb")), draw.unconnected_from_drc(B / name / "T0-50000000/routed.drc.json")),
        ("human", kgeom.load(str(B / name / "human/routed.kicad_pcb")), ()),
    ):
        x0, y0, x1, y1 = draw.bounds(ref)
        w, h = x1 - x0, y1 - y0
        fig = plt.figure(figsize=(6.2 if w < h else 6.2, 6.2 * (h / w) if w >= h else 6.2 * min(h / w, 1.5)))
        ax = fig.add_axes([0.01, 0.01, 0.98, 0.98])
        draw.draw(board, ax, labels=LABELS[name] if tag == "place" else (), unconnected=unc, window=(x0 - 1, y0 - 1, x1 + 1, y1 + 1), mark_scale=1.2, label_size=7)
        fig.savefig(OUT / f"{short}_{tag}.pdf")
        fig.savefig(OUT / f"{short}_{tag}.png", dpi=170)
        plt.close(fig)
        print(short, tag, "ok")
