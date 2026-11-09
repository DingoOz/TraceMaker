# Issue #56 records: repair windows (D172)

Doc 05 §75. Engine: branch `issue56-repair-window`, 2026-11-08, shared machine, `--no-kb`, one variant, CPU or GPU fields (same result).

## Staged flow on PCBench quick-tier boards

`staged.sh BOARD 1000000 off` makes stage 1 from `unrouted.kicad_pcb`: every net but the power nets
(`--skip-net 'GND*,*GND,VCC*,VDD*,+*,*3V3*,*5V*,*VBUS*,*VIN*'`, 1 M work, `--no-rescue`). `arm.sh BOARD NAME WORK MODE` routes stage 2
from it with that copper fixed. Of the 30 quick-tier boards, these nine leave connections open in stage 2 at 5 M work.

Arms (files `staged/<board>/s2-<arm>.{log,json}`; KiCad 10.0.6 DRC with zones refilled in `s2-<arm>.drc.json`, summary `kicad-drc.txt`):

- `off5`: `--work 5000000 --repair off --no-rescue`
- `auto5`: `--work 5000000` (repair `auto`), `--no-rescue`
- `offeq`: `--repair off --no-rescue`, `--work` = 5 M + the work `auto5`'s repair used
- `dfltoff` / `dflt`: as a plain run (relax rescue on), `--repair off` / `auto`

| Board (connections) | off5 | auto5 | offeq | Attempts (kept) | Input tracks / vias lifted | dfltoff | dflt |
|---|--:|--:|--:|--:|--:|--:|--:|
| 655_testboard (32) | 25 | 32 | 27 | 5 (1) | 55 / 8 | 27 | 32 |
| breakout-boards_swd-and-uart (4) | 3 | 4 | 3 | 2 (1) | 13 / 0 | 4 | 4 |
| custom_cpu--ALU_custom_cpu--ALU (39) | 30 | 39 | 39 | 4 (4) | 21 / 1 | 35 | 39 |
| fan_controller_fan_controller (69) | 66 | 69 | 66 | 1 (1) | 12 / 1 | 66 | 69 |
| Hangul-Clock_Hangul (169) | 160 | 168 | 169 | 5 (3) | 3 / 0 | 162 | 169 |
| Inkjet__autosave-InkjetDriver (39) | 38 | 39 | 39 | 1 (1) | 4 / 0 | 39 | 39 |
| LogicBoxen_LogicBoxen (64) | 60 | 64 | 62 | 1 (1) | 21 / 1 | 64 | 64 |
| mosavr_pcie1x_backplane (54) | 48 | 53 | 54 | 10 (4) | 10 / 0 | 54 | 54 |
| uC3Moy_uC3Moy (17) | 16 | 16 | 17 | 3 (0) | 0 / 0 | 16 | 16 |
| **total (487)** | **446** | **484** | **476** | | | **467** | **486** |

KiCad's DRC errors are the same for off5, auto5 and offeq on every board (the boards' own outline, courtyard, mask-bridge, annular and
edge errors); the unconnected counts equal the open connections.

## The PoE switch (the issue's board; private, not committed)

`/home/dingo/WholeBoardProjects-archive/20261027-015953-38a2-i-would-like-an-8-port-ethernet-switch-2-10port/rev6/poe_gbe_switch_10p-3-routed.kicad_pcb`
(1186/1189 in WholeBoard's run), with its `.kicad_pro` and `.kicad_dru`. `--work 2000000 --variants 1 --no-rescue`:

| Run | Routed | Repair | Log |
|---|--:|---|---|
| `--repair off` | 2/6 | | `poe/poe6-off.log` |
| default (windows 1, 2, 4 mm, 3 M per attempt) | 2/6 | 5 attempts, 0 kept (26-40 branches in a 1 mm window, > 64 at 2 mm) | `poe/poe6-auto.log` |
| `--repair-window-mm 0.2,0.4,0.7 --repair-work 10000000` | 2/6 | 6 attempts, 0 kept (9-22 branches; joined pads came apart each time) | `poe/poe6-small.log` |

All four open connections are at the KSZ9897 QFNs; the repair does not finish this board.
