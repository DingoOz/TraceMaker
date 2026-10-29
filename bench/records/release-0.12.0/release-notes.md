TraceMaker 0.12.1 is faster. It also checks differential pairs and delay tuning the way KiCad 10 does, and it can add a ground pour or change the layer count for you. At the same work budget it uses 20–30 % less CPU, and on hard boards the old router needs 1.4–2.6× the time to reach the same result.

This is the first published 0.12 release. The `v0.12.0` tag failed its install check (the example board needed four router variants, not two, since D143) and was not released. The results below were measured on the 0.12.0 build, which differs from 0.12.1 only in the version string, the install check and the documentation.

## Install without building (Linux x86_64)

- `tracemaker-0.12.1-linux-x86_64.tar.gz`: the router, the placer, the live viewer, an example board and the agent skills. Start with its `INSTALL.txt`.
- `tracemaker-0.12.1-pcm-linux-x86_64.zip`: the KiCad 10 plugin with the router inside. KiCad > Plugin and Content Manager > Install from File...

They need glibc 2.38 or newer (Ubuntu 24.04, Debian 13, current Fedora). They need no CUDA and no other library. CI installs them in a clean container with no compiler and routes the example board (D121).

## New

- **Faster search** (D154, #59):
  - A strict search that has run for 100,000 expansions now checks from both ends whether a path can exist at all. If none can, it stops. On hard boards, failed searches had been using 77–91 % of the first pass.
  - These parts are faster with exactly the same output, and each has a test against the old code:
    - the CPU cost-to-go field is vectorised (about 2×);
    - the inside-board test reads only the outline edges near the point (a LeeChee `--global` run went from 158 s to 7.5 s);
    - the per-class obstacle caches are allocated as they are used (Teensy: 1.6 GB to 0.56 GB per router).
- **Pours and layer count** (M20 C3, D138):
  - `route --pour auto|NET@LAYER` adds a pour to a board that has no zones.
  - `route --copper-layers N` and `tracemaker layers` change the copper layer count.
  - `route --try-layers 2,4,6` routes each count, fewest first, and keeps the first that completes.
  - On 28 of 28 boards, the layer advisor gave the same answer as a brute force judged by KiCad.
- **Unfilled zones are planes** (D140, #51): a zone written without a fill is filled in memory before routing (`--no-fill-planes` turns this off). Custom `hole_clearance` rules, such as JLCPCB's 0.254 mm hole-to-copper rule, are now kept by the router.
- **KiCad 10 parity on pairs and tuning** (D145, D147–D152):
  - Time-domain length matching is measured as KiCad 10.0.6 measures it.
  - KiCad's diff-pair coupling check is in the DRC, and the router avoids its coupling-gap errors (RoyalBlue54L: 5 errors to 0).
  - Pairs are named as KiCad names them.
  - Clearances are resolved in KiCad's order. On vme-wren, 22,362 false clearance reports dropped to none, matching KiCad.
  - KiCad 10 tuning profiles set widths per layer.
  - Delay tuning rips up and re-routes the connections a meander needs room from.
- **`tracemaker causes`** (D143): lists the open connections by cause. Half the portfolio now searches from the crowded end, and the open connections on the failing set fell from 1,383 to 1,220.
- **`--si-rules`** stitches the layer changes of critical nets and reports where it could not (D144, D146).
- **Fixes:**
  - Starved thermal reliefs after a soft-zone refill are found and repaired pad by pad (D137).
  - When the shared GPUs are full, the router backs off instead of asking the driver on every field. This was the cause of the 20-minute `interf_u` stall listed in 0.11.0's known limits (2,900 s, against 20 s without the GPU).
  - Custom rules keep the fast per-class cache unless it would judge them differently (D139).
- **KiCad drift job** (D142): every week CI tries the newest KiCad image. It upgrades, re-reads, judges and routes four boards.

## Results

PCBench, 8 router variants, 120 s per board, judged by KiCad's own DRC (`kicad-cli pcb drc`, the D106 judge, the same as 0.10.0 and 0.11.0). Measured on an idle machine.

| Tier | Boards | 0.12.1 clean | 0.11.0 clean | Freerouting 2.5.0-RC12 (published) | 0.12.1 completion |
|---|--:|--:|--:|--:|--:|
| A | 40 | **100%** | 100% | 100% | 100% |
| B | 40 | **72.5%** | 70.0% | 50.0% | 97.4% |
| C | 30 | **50.0%** | 46.7% | 46.7% | 97.5% |
| D | 22 | **40.9%** | 36.4% | 36.4% | 95.9% |

One more board is clean in each of tiers B, C and D: avr_ledprojector, kitspace_40-channel-hv-switching-board and Keyboard_PCB_Keyboard (534 of 534). On most boards that stay open there are fewer open connections, for example CoreOne 56 → 28, LeeChee 46 → 29 and TCKB 31 → 18. No board has an error added by routing. 0.11.0 was measured on a shared machine and the limit is wall-clock time. For a like-for-like check, 0.11.0 on an idle machine scored 8 tier D boards (2026-10-26); 0.12.1 scores 9.

Records: `bench/records/release-0.12.0/` (every run's summary and per-board results, the comparisons, the test log).

Test suite: 428 tests, all passing in the release build. One skips without its fixture. `delay_room` fails when run with the prepared demo boards (see Known limits).

## Known limits

- The gates of 0.8.0 are still not met: from-scratch placement (85 % against 90 %) and dense-package boards (41 % clean).
- The agent interface and the advisor are experimental (neither beat the plain router at equal compute).
- **Delay tuning on one test board** ([#60](https://github.com/DingoOz/TraceMaker/issues/60)): on RoyalBlue54L-Feather with its USB skew rule, a stage after tuning shortens /Debugger/D+ again, leaving it 8.7 ps outside a 0.5 ps limit. TraceMaker's own DRC reports the violation. 0.11.0 stayed inside the limit on that board because it routed a different board (125 against 129 connections). Check boards with skew or delay rules with `tracemaker drc` or KiCad's DRC.
- Pours do not close boards on their own: on the 33 non-clean tier B–D boards, none became clean with a pour.
- The KiCad plugin and the Python module do not pass `--report` yet. The report's DRC is TraceMaker's own (it uses KiCad's violation names), so sign off with `kicad-cli pcb drc`.
- The release files are Linux x86_64 only.
