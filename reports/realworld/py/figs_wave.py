# SPDX-License-Identifier: GPL-3.0-or-later
import json, pathlib, sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
RW = pathlib.Path("/home/dingo/Programming/TraceMaker/reports/realworld")
def load(b, case, lay):
    z = np.load(RW / "sim/results" / b / f"{case}.{lay}.npz")
    j = json.load(open(RW / "sim/results" / b / f"{case}.{lay}.json"))
    return z, [p["label"] for p in j["probes"]]
fig, axs = plt.subplots(2, 2, figsize=(7.4, 4.6))
for col, (b, case, labs, lays, title) in enumerate((
    ("kitspace_teensy-fx", "audio", ["BCLK at the codec", "LRCLK (quiet low) at the codec"], [("human", "Designer"), ("B", "Ours B")], "Teensy: I2S"),
    ("Hardware_Playground_serial_gw_ATMEGA328P", "spi_usb", ["SCK at the level shifter", "slave select at the level shifter"], [("human", "Designer"), ("C", "Ours C")], "Gateway: SPI"))):
    for row, lab in enumerate(labs):
        ax = axs[row][col]
        for lay, name in lays:
            z, L = load(b, case, lay)
            i = L.index(lab)
            t, v = z[f"t{i}"] * 1e9, z[f"v{i}"]
            m = (t > 600) & (t < 1000)
            ax.plot(t[m], v[m] * (1e3 if row == 1 else 1), label=name, lw=0.9)
        ax.set_title(f"{title}: {lab}", fontsize=7)
        ax.set_ylabel("mV" if row == 1 else "V", fontsize=7)
        ax.tick_params(labelsize=6)
        if row == 1: ax.set_xlabel("ns", fontsize=7)
        ax.legend(fontsize=6)
fig.tight_layout()
fig.savefig(RW / "figs/waves.pdf"); fig.savefig(RW / "figs/waves.png", dpi=150)
print("waves ok")
