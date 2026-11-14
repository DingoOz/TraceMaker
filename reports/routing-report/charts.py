# SPDX-License-Identifier: GPL-3.0-or-later
"""Charts for the routing report from the runs' JSON summaries and recordings (build/report-venv python)."""
import json, pathlib

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = pathlib.Path(__file__).resolve().parent
RUNS, FIGS = HERE / "runs", HERE / "figs"
plt.rcParams.update({"font.size": 10, "axes.spines.top": False, "axes.spines.right": False, "figure.dpi": 150})
BLUE, ORANGE, GREEN, GREY, MAG = "#3b6fd6", "#e08a1e", "#2e9e5b", "#9aa3b0", "#c437b0"
BOARDS = (("hangul", "Hangul-Clock"), ("ethercat", "EtherCAT shield"), ("plane_smd", "plane_smd"))


def load(b):
    return json.load(open(RUNS / f"{b}.json"))


def variants():
    fig, axes = plt.subplots(1, 3, figsize=(10, 3.2), sharey=False)
    for ax, (b, name) in zip(axes, BOARDS):
        j = load(b)
        vs = j["variants"][:8]  # the plain portfolio; rescue passes append theirs
        n = j["connections"]
        ys = [v["routed"] for v in vs]
        cols = [GREEN if v.get("best") else BLUE for v in vs]
        ax.bar(range(len(vs)), ys, color=cols)
        ax.axhline(n, color=GREY, ls="--", lw=1)
        lo = min(ys) - max(2, (n - min(ys)) * 0.6)
        ax.set_ylim(max(0, lo), n + max(1, (n - lo) * 0.08))
        ax.set_xticks(range(len(vs)))
        ax.set_title(f"{name} ({n} connections)", fontsize=10)
        ax.set_xlabel("variant")
    axes[0].set_ylabel("connections routed")
    fig.tight_layout()
    fig.savefig(FIGS / "chart_variants.pdf")


def progress():
    fig, ax = plt.subplots(figsize=(10, 3.4))
    for (b, name), c in zip(BOARDS, (BLUE, ORANGE, MAG)):
        st = [json.loads(l) for l in open(RUNS / f"{b}.jsonl") if '"stats"' in l]
        st = [e for e in st if e.get("stage") == "route"]
        if not st:
            continue
        tot = st[0]["total"]
        best, ys = 0, []
        for e in st:  # restarts rip everything: plot the best state so far, which is what the variant returns
            best = max(best, e["routed"])
            ys.append(100 * best / tot)
        ax.plot([e["t"] for e in st], ys, color=c, lw=1.4, label=name)
    ax.set_xscale("symlog", linthresh=1)
    ax.set_xlabel("seconds since the winning variant started (log scale above 1 s)")
    ax.set_ylabel("% routed (best state so far)")
    ax.set_ylim(0, 101)
    ax.legend(frameon=False, loc="lower right")
    fig.tight_layout()
    fig.savefig(FIGS / "chart_progress.pdf")


def ladder():
    """Where the time and the connections went in each default run: plain portfolio, warm rescue, fresh rescue."""
    fig, axes = plt.subplots(1, 2, figsize=(10, 3.2))
    names, plain, warm, fresh, tp, tw, tf = [], [], [], [], [], [], []
    for b, name in BOARDS:
        j = load(b)
        r = j.get("rescue") or {}
        ps = r.get("passes", [])
        base = r.get("routed_before", j["routed"])
        names.append(name)
        plain.append(base)
        w = next((p for p in ps if p["mode"] == "warm"), None)
        f = next((p for p in ps if p["mode"] == "fresh"), None)
        warm.append(w["routed"] if w else None)
        fresh.append(f["routed"] if f else None)
        tot = j["seconds"]
        tw.append(w["seconds"] if w else 0)
        tf.append(f["seconds"] if f else 0)
        tp.append(max(0, tot - tw[-1] - tf[-1]))
    ax = axes[0]
    for i, n in enumerate(names):
        c = load(BOARDS[i][0])["connections"]
        pts = [("plain", plain[i]), ("warm", warm[i]), ("fresh", fresh[i])]
        xs = [k for k, (lab, v) in enumerate(pts) if v is not None]
        ys = [100 * pts[k][1] / c for k in xs]
        ax.plot(xs, ys, marker="o", label=n)
    ax.set_xticks([0, 1, 2], ["plain portfolio", "rescue: warm", "rescue: fresh"])
    ax.set_ylabel("% routed (kept result)")
    ax.legend(frameon=False, fontsize=8)
    ax.set_title("connections after each stage", fontsize=10)
    ax = axes[1]
    ax.barh(names, tp, color=BLUE, label="plain portfolio")
    ax.barh(names, tw, left=tp, color=ORANGE, label="rescue: warm")
    ax.barh(names, tf, left=[a + b for a, b in zip(tp, tw)], color=MAG, label="rescue: fresh")
    ax.set_xlabel("wall seconds (default --time 120 per stage)")
    ax.legend(frameon=False, fontsize=8)
    ax.set_title("where the time went", fontsize=10)
    fig.tight_layout()
    fig.savefig(FIGS / "chart_ladder.pdf")


def why_open():
    fig, ax = plt.subplots(figsize=(10, 2.4))
    keys = (("walled_in", "walled in (proved)"), ("neck_down", "only the class width"), ("soft_only", "only zone fills"),
            ("congested", "congestion"))
    names, rows = [], []
    for b, name in BOARDS:
        w = load(b).get("why_open") or {}
        if not w.get("pins"):
            continue
        names.append(name)
        rows.append([w.get(k, 0) for k, _ in keys])
    left = [0] * len(names)
    for i, ((k, lab), c) in enumerate(zip(keys, (MAG, ORANGE, GREEN, GREY))):
        vals = [r[i] for r in rows]
        ax.barh(names, vals, left=left, color=c, label=lab)
        left = [a + v for a, v in zip(left, vals)]
    ax.set_xlabel("pins of open connections")
    ax.legend(frameon=False, ncol=4, fontsize=8, loc="lower center", bbox_to_anchor=(0.5, 1.0))
    fig.tight_layout()
    fig.savefig(FIGS / "chart_why_open.pdf")


def budget():
    rows = []
    for w in (1_000_000, 3_000_000, 10_000_000, 30_000_000):
        f = RUNS / f"eth_w{w}.json"
        if f.exists():
            j = json.load(open(f))
            r = j.get("rescue") or {}
            rows.append((w, r.get("routed_before", j["routed"]), j["routed"], j["seconds"], j["connections"]))
    if not rows:
        return
    d = load("ethercat")
    fig, axes = plt.subplots(1, 2, figsize=(10, 3.2))
    ax = axes[0]
    ax.plot([r[0] for r in rows], [r[1] for r in rows], marker="o", color=BLUE, label="plain portfolio")
    ax.plot([r[0] for r in rows], [r[2] for r in rows], marker="s", color=GREEN, label="after the rescue")
    ax.axhline(d["routed"], color=MAG, ls="--", lw=1, label=f"default (--time 120): {d['routed']}")
    ax.set_xscale("log")
    ax.set_xlabel("--work (expansions per variant)")
    ax.set_ylabel(f"routed of {rows[0][4]}")
    ax.legend(frameon=False, fontsize=8)
    ax = axes[1]
    ax.plot([r[0] for r in rows], [r[3] for r in rows], marker="o", color=ORANGE)
    ax.axhline(d["seconds"], color=MAG, ls="--", lw=1, label=f"default run: {d['seconds']:.0f} s")
    ax.set_xscale("log")
    ax.set_xlabel("--work (expansions per variant)")
    ax.set_ylabel("wall seconds")
    ax.legend(frameon=False, fontsize=8)
    fig.suptitle("EtherCAT shield: deterministic work budgets against the default time limit", fontsize=10)
    fig.tight_layout()
    fig.savefig(FIGS / "chart_budget.pdf")


if __name__ == "__main__":
    variants(); progress(); ladder(); why_open(); budget()
    print("charts written")
