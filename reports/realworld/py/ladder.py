#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Rule-escalation ladder: route under the board's own rules (T0); the connections that do not fit are routed in a second stage under looser rules (T1), the
rest in a third (T2), each stage warm-started from the copper of the one before (legal under the looser rules by construction). Compromises then sit only
where a connection needed them.

  ladder.py BOARD[+planes] NAME STAGES [route options...]      STAGES like T0:20000000,T1:20000000,T2:50000000
  With --keep-power a stage may name a second tier for the power nets, SIGNAL/POWER:work (T2/T1:50000000); POWER defaults to T0 (the board's own rules, never
  narrowed). Power relaxes last: signals take the whole ladder first, then power takes one tier at a time, only while the board is still open.
  writes reports/realworld/boards/BOARD/NAME/stage-k/ and NAME/routed.kicad_pcb (the final copper on a copy with the board's own rules)
"""
import pathlib
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import rules

ROOT = pathlib.Path(__file__).resolve().parents[3]
FIX = ROOT / "bench/data/freerouting/scripts/benchmark/fixtures/PCBench"
TM = ROOT / "build/release/src/app/tracemaker"


def main() -> int:
    board, name, stages = sys.argv[1], sys.argv[2], sys.argv[3].split(",")
    extra = sys.argv[4:]
    keep_supply = "--keep-supply" in extra  # as --keep-power, but ground nets may take the relaxation
    keep_power = "--keep-power" in extra or keep_supply  # power and ground nets stay at the board's own rules and are never narrowed: the relaxation is spent on signals only
    extra = [e for e in extra if e not in ("--keep-power", "--keep-supply")]
    planes = board.endswith("+planes")
    board = board.removesuffix("+planes")
    if planes:
        extra = ["--soft-zones"] + extra
    out = ROOT / "reports/realworld/boards" / board / name
    out.mkdir(parents=True, exist_ok=True)
    src = ((ROOT / "reports/realworld/boards" / board / "planes.kicad_pcb") if planes else (FIX / board / "unrouted.kicad_pcb")).read_text()
    power = rules.power_nets(src, ground=not keep_supply) if keep_power else []
    if power:
        extra = extra + ["--no-neck-nets", ",".join(n for n in power if "," not in n)]
        print(f"{board[:24]} power nets kept at the board's rules ({len(power)}): {' '.join(power[:12])}{' ...' if len(power) > 12 else ''}", flush=True)
    tiers = dict(rules.TIERS)
    tiers["T0"] = rules.own_tier(src)  # stage 0 is the board's own rules, whatever they are (robomezzi: 0.178 mm clearance, 0.61/0.30 vias)
    print(f"{board[:24]} own rules {tiers['T0']}", flush=True)
    (out / "stages.txt").write_text(",".join(st.split(":")[0] for st in stages) + "\n")  # the dev page labels its columns from this
    warm = None
    for k, st in enumerate(stages):
        tier, work = st.split(":")
        tier, _, ptier = tier.partition("/")
        ptier = ptier or "T0"
        d = out / f"stage-{k}-{tier}"
        d.mkdir(exist_ok=True)
        (d / "relaxed.kicad_pcb").write_text(rules.apply(src, tiers[tier], keep=power, keep_t=None if ptier == "T0" else tiers[ptier]))
        stage_extra = extra
        if power and ptier != "T0":  # power is relaxed in this stage: it may be narrowed too (the last resort), so the strict-neck option goes
            stage_extra = [e for k2, e in enumerate(extra) if e != "--no-neck-nets" and (k2 == 0 or extra[k2 - 1] != "--no-neck-nets")]
        cmd = [str(TM), "route", str(d / "relaxed.kicad_pcb"), "-o", str(d / "routed_relaxed.kicad_pcb"), "--work", work, "--threads", "8", "--no-gpu", "--no-kb"] + stage_extra
        if warm:
            cmd += ["--warm-pcb", str(warm)]
        with open(d / "route.log", "w") as f:
            subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT)
        warm = d / "routed_relaxed.kicad_pcb"
        line = next((l for l in (d / "route.log").read_text().splitlines() if l.startswith("routed ")), "no result")
        print(f"{board[:24]} {name} stage {k} {tier}/{ptier if power else tier} {work:>9s}: {line[:100]}", flush=True)
        if line.startswith("routed") and line.split()[1].split("/")[0] == line.split()[1].split("/")[1]:
            break
    (out / "routed.kicad_pcb").write_text(rules.apply(warm.read_text(), tiers["T0"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
