# Test campaign of October 2026: soak, full tests, and the fixes (D137)

Release 0.11.0 (`main` `2adbac9`) was tested twice on this machine (56 cores, Tesla P100 and V100), with no code changed while testing:

- **Overnight soak, 2026-10-25** ([soak-report-2026-10-25.md](soak-report-2026-10-25.md)): tricky boards (the 33 PCBench tier B-D
  boards 0.9.0 left open, the plane tier, failing srj20 boards) at about 24 threads beside other jobs, with both GPUs full.
- **Full campaign, 2026-10-26** ([full-test-2026-10-26.md](full-test-2026-10-26.md)): the whole machine and both GPUs free; test
  suites in every preset, GPU against CPU fields, the release tiers, tscircuit and PCBWorld at their recorded recipes.

Every byte-identity check passed (CPU, P100, V100 and both GPUs; thread counts; work budgets; srj19 and srj20 200/200 against the
18 October runs), and the published figures reproduced (tiers A-D, srj19 178/200, srj20 129/200, PCBWorld D3 116/119). The run
directories named in the reports are in `bench/results/` (git-ignored) on the test machine.

## What was found, and what changed

| Finding | Change |
|---|---|
| `--soft-zones` adds KiCad `starved_thermal` errors on interf_u (6 per run), which the router neither saw nor reported | `drc::starved_thermals` (KiCad 10's rules), reported by `tracemaker drc` and by soft-zone routes, and a thermal repair per starved pad (doc 05 §36.1, D137) |
| With full GPUs every field queued on the CUDA driver lock: interf_u 2,900 s against 20 s, identical boards | A device that refused is left alone for 30 s, doubling to 15 min (`gpu/backoff.hpp`, doc 07 §4, D137) |
| `Copper text boxes cover KiCad's stroke-font advances` read `classes.front()` of an empty list (fails under ASan and TSan) | The test adds the Default class the project reader always adds |
| `round_options` exceeds its 600 s limit under TSan (555 s under ASan) | Limit 1800 s |
| `kicad_drc_broken_parity` fails when two presets' suites run at once: one scratch directory for all | `build/scratch/broken/<preset>/` per binary |
| `bench/viewport.py` ran out of its 100 viewer ports on fast board sets (claims held 5 minutes) | `run.py` releases a board's port when its route ends |
| `run.py --view` cannot drive binaries before 0.11.0 (`--view-embed`) | `run.py` passes `--view-embed` only when the binary's help lists it |
| `run.py` crashed on its summary when every board errored | Empty rates print as "–"; such a run exits 1 |
| The "standard run" line of `bench/sets/srj19.txt` and `srj20.txt` (10 M work) is not the recorded recipe | 30 M work with `--fast-bends` (doc 10 §9), README too |
| `--relax` ignores `--work`, silently | Its help says so, and the log says so when both are given |
| The release record put tier D's one board on load; idle, both binaries score 8 | `release-0.11.0/README.md` §3 and the release notes corrected: the 120 s wall-clock limit, not the code |
| (found while writing the records) the route summary kept `--view-token` in `run.options`, so a shared summary or report opened the viewer | Recorded as "(not recorded)" |

Also cleaned up: the 16 runs the soak's first interf_u repeats wrote to the owner's knowledge base without `--no-kb` (backup kept
beside it). Left open: the GPU path is slower than CPU fields on short runs even with free GPUs (+21 to +138 % route CPU time;
-15 % only at 30 M work with three boards on two GPUs); whether to make `--no-gpu` the default is a separate decision.

## Checks of the changes

- `ctest --preset release`: all tests pass (one skipped: no fixture). Under ASan the stroke-font test and the new `[thermal]` and
  `[backoff]` tests pass.
- **quick tier** (30 boards, 1 M work, GPU fields, viewer on): 30/30 byte-identical to `full-g-quick`
  ([compare-fix-quick.txt](compare-fix-quick.txt), [runs/fix-g-quick](runs/fix-g-quick)). The viewer claims file was empty afterwards.
- **plane tier** (8 boards, 3 M work, `--soft-zones --si-rules`, refilled before judging): 7/8 byte-identical to `full-g-planes`;
  interf_u changed: KiCad `starved_thermal` 6 → 4, 140/164 routed and 26 unconnected either way, route time 15 → 76 s
  ([compare-fix-planes.txt](compare-fix-planes.txt), [runs/fix-g-planes](runs/fix-g-planes), the summary in
  [interf_u-thermal/route.json](interf_u-thermal/route.json) and a route log of the same command with `--no-gpu` in
  [interf_u-thermal/route-log.txt](interf_u-thermal/route-log.txt)). Commands: [fixq.sh](fixq.sh).
- **Parity of the starved-thermal check** with kicad-cli 10.0.6 (`thermal_parity.py <run>/boards`, KiCad's reports from the DRC
  cache): before the fix, KiCad's 6 pads on interf_u and one more (U9.C4), none on the other seven boards
  ([thermal-parity-full-g-planes.txt](thermal-parity-full-g-planes.txt)); after it, the same 4 pads as KiCad
  ([thermal-parity-fix-g-planes.txt](thermal-parity-fix-g-planes.txt)). Run on KiCad's own fills of the routed interf_u it gives the
  same pads, so the check, not the refill, accounts for U9.C4. Projects that set `min_resolved_spokes` to 1 (pic_programmer,
  RoyalBlue54L-Feather, sonde xilinx) gave 7, 20 and 2 false reports until the setting was read.
