# D160 records: the relax rescue by default

Results behind [doc 05 §68](../../../docs/05-routing.md) and [D160](../../../docs/decisions/D160.md). Branch `relax-rescue` (on `thin-vias`,
D159), 2026-10-30. Machine: 56 cores, shared. No GPU (`--no-gpu`).

## 1. Default runs end on their time limit (`default-time/`)

`tracemaker route BOARD --no-kb --threads 8` (no `--work`, so the default 120 s limit) on four hard PCBench boards:

| Board | Routed | Wall | Ended by |
|---|--:|--:|---|
| kitspace_ardfpga | 206/214 | 124 s | time limit |
| kitspace_teensy-fx | 192/196 | 124 s | time limit |
| poncho_fpga_fpga-dongle | 387/413 | 125 s | time limit (2 passes) |
| serial_gw ATMEGA328P | 171/172 | 122 s | time limit |

The plain portfolio is still working when the limit comes, so there is no time left over for a rescue. The rescue gets `--time` again.

## 2. The 33 failing PCBench tier B-D boards (`runs/rescue-bd`, `runs/compare-33.md`)

`bench/run.py --set pcbench-bd-fail --work 100000000 --jobs 5 --threads 8 --time 3600 --route-args=--no-gpu`: the default, so the rescue
runs on every board left open, with 5 rounds of 20 M work.

| Arm | Routed (of 16,935) | vs plain | Better / worse | KiCad-clean | CPU s |
|---|--:|--:|--:|--:|--:|
| plain (`thin-bd-base8`, main) | 16,166 | | | 2 | 64,062 |
| `--relax` alone (`thin-bd-relax-final9`, D159) | 15,925 | −241 | 8 / 15 | 5 | 60,232 |
| **default with the rescue** (`rescue-bd`) | **16,208** | **+42** | **8 / 0** | **5** | 110,958 |

- Every board is the better of the plain run and relax, as designed: the per-board counts equal the maximum of the two arms on all 33.
  The plain part is byte-for-byte the plain arm's (`routed_before` equals the plain arm's count on every board).
- The rescue ran on 30 boards (the plain run completes the other 3: multisensor_cr2032, 58r-mxalps and LeeChee_1800, which is
  complete but not clean) and was kept on 12. Eight of those
  routed more; four routed as many with fewer vias or shorter copper (fifogfx 64, test-jig 260, sd_wav 394, robomezzi 280).
- Gains: serial_gw 171 → 172 (clean), ardfpga 209 → 214 (clean), teensy-fx 191 → 196 (clean), EtherCAT 245 → 247, V2X 461 → 467,
  MonApollo 1148 → 1153, CoreOne 787 → 804, dorkyboard 826 → 827.
- No board added a DRC error. CPU: +73 % on this set, where every board is left open; a board that completes pays nothing.
- A rescue took 99-766 s of wall time here (5 jobs of 8 threads on the shared machine).

## 3. Quick tier (`runs/rescue-quick`, `quick-compare.md`)

`bench/run.py --set quick --work 1000000 --route-args "--variants 1" --jobs 4`, against `thin-quick-main` (main):

- 19 of 30 boards are left open at this budget; the rescue ran on all 19 and was kept on 8.
- +11 connections; clean boards 11 → 12 (C-BISCUIT completes); 22 of 30 boards byte-identical. **Gates: pass.**
- Each rescue took 1-3 s.

## 4. Test

`tests/integration/round_options.py`: with connections left open the rescue runs and is reported; `--no-rescue` turns it off; a rescue
that does not win leaves the output byte-identical to the plain run's; the result is never below the plain run. Release and ASan pass
(on its board the rescue goes 23 → 27 routed and is kept).

