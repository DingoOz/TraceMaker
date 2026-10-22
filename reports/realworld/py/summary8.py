#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""summary8.py: the best closure run of each of the 8 boards under test (fewest open in the loop's own judging, then fewest violations), with its KiCad ledger and
safety verdict where they exist. Prints markdown and writes bench/records/closure/8boards.md."""
import json
import pathlib

RW = pathlib.Path(__file__).resolve().parents[1]
BOARDS = ["96boards-robomezzi_96boards-robomezzi", "DronPi_dronPi", "Hardware_Playground_E3_Pcb_rev0.3", "EtherCAT_shield_v1_EtherCAT_shield_v1", "kitspace_test-jig",
          "poncho_fpga_fpga-dongle", "PocketBone_pocketbone-kicad", "Own-Mailbox-Hardware_mailbox-before"]


def main() -> int:
    rows = []
    for b in BOARDS:
        best = None
        for d in sorted((RW / "boards" / b).glob("closure-*")):
            cj = d / "closure.json"
            if not cj.exists():
                continue
            c = json.loads(cj.read_text())
            rd = [r for r in c["rounds"] if r["kind"] in ("route", "effort", "fine", "window", "escape", "shove") and "open" in r]
            if not rd:
                continue
            m = min(rd, key=lambda r: (r["open"], r.get("violations", 1e9)))
            key = (m["open"], m.get("violations", 1e9))
            led = json.loads((d / "ledger.json").read_text()) if (d / "ledger.json").exists() else None
            sj = RW / "sim/results" / b / f"safety.{d.name}.json"
            saf = json.loads(sj.read_text()).get("verdict") if sj.exists() else None
            shorts = sum(1 for x in led["added"] if x["type"] == "shorting_items" and any(i.startswith(("Track", "Via", "Arc")) for i in x.get("items", []))) if led else 0  # a track touching copper of another net: a short, never a compromise
            verified = led is not None and led["unconnected"] == 0 and saf and saf != "FAIL" and shorts == 0
            cand = (not verified, saf != "PASS", key, d.name, m["open"], led, (saf + (f" + {shorts} SHORTS" if shorts else "")) if saf else saf, len(c["assign"]), sum(1 for n in c["assign"] if n in c["power"]), len(c.get("hard", [])), len(c["moves"]))
            if best is None or cand[:3] < best[:3]:
                best = cand
        if best:
            _, _, _, run, op, led, saf, rel, relp, hard, mv = best
            rows.append(f"| {b[:36]} | {run} | {led['unconnected'] if led else op} | {len(led['added']) if led else ''} | {rel} ({relp}) | {hard} | {mv} | {saf or ''} |")
    out = ["| board | best run | open (KiCad if judged, else the loop's) | compromises | nets relaxed (power) | pours hard | parts moved | safety |", "|---|---|--:|--:|--:|--:|--:|---|"] + rows
    text = "\n".join(out) + "\n"
    (RW.parents[1] / "bench/records/closure/8boards.md").write_text(text)
    print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
