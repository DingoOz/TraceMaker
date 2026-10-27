# Overnight tests on tricky boards, 2026-10-25

Started 21:53 UTC at the owner's "run tests on tricky boards until I say stop (about 7 hours), without using all CPUs".
Binary: release 0.11.0 (main `2adbac9`), `build/release`. Results in `bench/results/soak-1025/` and `bench/results/soak-*` (git-ignored).

## Summary (04:50 UTC)

No crash, hang, sanitizer report or data race in any run; determinism held everywhere it was tested. Three things to act on:

1. **GPU fallback cliff (performance, the biggest).** With both GPUs full (other jobs), every cost-to-go field first asks the
   driver for free memory and waits on its lock before falling back to the CPU. Same boards, **10-300x slower** (interf_u
   2,900 s vs 20 s; ErgoDone 1,708 s vs 6 s), output byte-identical. This is the interf_u "stall" of the M20 records. Suggested fix:
   stop asking the GPU for the rest of the run after the first out-of-memory result. (Phases 6, 6b, 1.)
2. **interf_u `starved_thermal` with `--soft-zones` (correctness, rule 1 in KiCad's eyes).** A new B.Cu track cuts the GND pour so
   U2 pad 10's thermal reaches only an island; the soft-zone model does not check thermals. +6 at 3 M work, every time. (Phases 3a, 3b.)
3. **`--relax` ignores `--work`** (round-based by design); worth one line in its help. (Phase 2a.)

Also: my first interf_u repeats ran without `--no-kb` and added rows to the owner's knowledge base (phase 6b).

| Test | Boards | Result |
|---|--:|---|
| Determinism, 3 M work, 3x8 vs 6x4 threads | 33 | 33/33 byte-identical |
| Determinism, 30 M work, 3x8 vs 6x4 threads | 33 | 33/33 byte-identical; completion 82.5 → 90.9 % |
| GPU fallback vs CPU fields | 1 + 25 | identical (except one board stopped by the time cap on the GPU path) |
| `--soft-zones --si-rules`, `--escape-flow`, seeds 2-4 | 33 each | 0 added errors; `--escape-flow` +78 connections |
| `--relax` | 9 + 12 srj20 | 3 tricky boards clean (serial_gw, multisensor, ardfpga); 12/12 srj20 boards clean |
| Plane tier, soft zones | 8 | interf_u `starved_thermal` +6 (real), StickHub mask bridge (judge noise) |
| ASan (planes, then 33 tricky boards) | 8 + 33 | no report |
| TSan (2, then 33 tricky boards) | 2 + 33 | no race |

## Ground rules I set

- **CPU:** at most about 24 router threads at a time (3 boards x 8 threads, or 6 x 4), everything under `nice`; the machine has 56 cores
  and other jobs (load about 22 before the start). The sanitizer builds run at `nice 15`, 12 compile jobs.
- **Boards:** "tricky" = the 33 PCBench tier B-D boards that 0.9.0 left unfinished (`bench/sets/pcbench-bd-fail.txt`), the plane tier
  (`planes.txt`, soft zones), and tscircuit's srj20 BGA boards a single variant leaves open (`srj20-m15fail.txt`).
- **Fixed work** (`--work`, `--time 3600` as a safety stop) wherever results are compared, so the shared machine cannot change them.
- Every run streams to a viewer (`--view`). Judge: kicad-cli 10.0.6 (D106).
- Tests only: no code is changed or pushed tonight. Findings are written here with how to reproduce them.

## What each phase checks

| Phase | What | Pass means |
|---|---|---|
| 1 | Determinism: `pcbench-bd-fail` at 3 M work, 3 x 8 threads, then 6 x 4 threads | byte-identical boards (thread count must not change the output) |
| 2 | Option stress on the same boards: `--relax`, `--txn 1`, `--soft-zones --si-rules`, the escape options | no crash, no KiCad error added (rule 1) |
| 3 | ASan build on the plane tier and the hardest failing boards | no sanitizer report |
| 4 | TSan build, a few boards, 8 threads | no data race report |
| 5 | srj20 BGA boards with `--relax` | no crash, no error added |
| 6 | `interf_u` post-portfolio stall (bench/records/m20-run-report) | reproduce, time it, find which stage |
| 7 | Router seeds 2-4 on the failing boards | no crash, no error added |

## Results

### Phase 6: the interf_u "stall" is one slow variant computing its fields on the CPU (not a hang)

Same command as the M20 records (`--work 3000000 --time 3600 --threads 4 --soft-zones --si-rules --view`), log lines time-stamped
(`bench/results/soak-1025/interf_u/log.txt`). It finished in **395 s** (exit 3, 140/164 routed):

| t (s) | What |
|--:|---|
| 3 | portfolio: 8 variants on 4 threads |
| 15-136 | seven variants finish (field time 5-109 s each) |
| 136-344 | **variant 6 alone** ("cheap crossings, escape plan"): 230 s, of which **223 s are cost-to-go fields on the CPU** (90 fields, 90 GPU fallbacks) |
| 344-394 | soft-zone refill: 33 unconnected, refill repair routes 7 (44 s of fields, 18 on the CPU) |

Both GPUs were full with other jobs (11.9 of 12 GB and 15.9 of 16 GB used), so every field fell back to the CPU, about 2.5 s each on
this board, single-threaded inside its variant. The 14 October record (27 s) had a GPU. On a busier machine (load 40-66 during the M20
runs) the same tail stretches to the 20+ minutes seen then. So: not a deadlock, but a performance cliff when the GPU is unavailable.
Possible fixes, not made: compute a variant's fields with the idle portfolio threads once other variants are done; cap field work per
variant on the CPU path; or say in the log when a variant is alone and field-bound.

### Phase 3a: ASan on the plane tier (`soak-asan-planes`), 1 M work, `--soft-zones --si-rules`, KiCad refills zones

No sanitizer report on any of the 8 boards (exit codes 0 and 3 only; ASan would exit 1, LeakSanitizer 23). The KiCad judge flags two
boards with added errors; both are old, not from 0.11.0:

- **StickHub, `solder_mask_bridge` +11:** documented judge noise (doc 10 §4: the count moves between refills of byte-identical boards;
  3, 4, 3 and 15 in the earlier soft-zone runs `i8`, `m16-*`, `i24j`).
- **interf_u, `starved_thermal` +1 (+6 in every earlier soft-zone run at 3 M work: `i8-planes-soft`, `m16-planes-soft-*`,
  `i24j-planes-soft-si`) is a real error the router adds.** KiCad: "Thermal relief connection to zone incomplete (bottom copper; 2 spokes
  connected to isolated island)", `PTH pad 10 [GND] of U2` at (118.75, 124.46). New B.Cu copper next to it: `/PC-A10` runs vertically at
  x = 120.1 (1.35 mm from the pad) through the bottom GND pour; with the other new tracks it cuts off the piece of pour the pad's spokes
  reach. The soft-zone refill model counts unconnected items but not a pad whose thermal reaches only an island, so the router's own
  check passes it. Doc 05 §(soft zones) says "no board gained a DRC error"; that holds for the engine's DRC, not for KiCad's
  `starved_thermal`. Reproduce: `bench/run.py --fixtures build/demos --set planes --work 3000000 --refill --route-args "--soft-zones"`,
  or `kicad-cli pcb drc --refill-zones` on `bench/results/soak-asan-planes/boards/interf_u.kicad_pcb` (records in
  `bench/results/soak-1025/interf_u-drc/`). Not fixed tonight.

### Phase 4: TSan (`soak-tsan`), 2 boards (serial_gw, E3), 300 k work, 8 threads, viewer on

No data-race report (exit 3 on both, "not all routed"; with `TSAN_OPTIONS=exitcode=66` a report would give 66). Small runs (32 and 62 s).

### Phase 6b: the GPU fallback gives the same board as the CPU path, about 150 times slower (rule 3 holds)

interf_u, `--work 3000000 --no-kb`, three runs each (`bench/results/soak-1025/det-interf/`):

| Runs | Board md5 | Routed | Wall time |
|---|---|--:|--:|
| `--no-gpu`, 8 threads | `66a5751a` (all) | 140 | 16-24 s |
| `--no-gpu`, 4 threads | `66a5751a` (all) | 140 | 24 s |
| GPU path, GPUs full, 4 threads | `66a5751a` (all) | 140 | 2,889-2,937 s |

Same output across repeats, across thread counts and across the GPU and CPU paths: no determinism problem. With both GPUs
full, though, every cost-to-go field first calls `has_free_memory()` (`src/gpu/device_cuda.cu`: `cudaSetDevice` + `cudaMemGetInfo`),
and the threads queue on the NVIDIA driver lock (seen in D state on `os_acquire_rwlock_*`) before falling back to `field_cpu`. That
wait, not the CPU field, is the cost. Suggested fix (not made): after the first out-of-memory result, stop asking the GPU for the rest
of the run (or back off for N fields).

**Mistake on my part:** the first interf_u repeats ran without `--no-kb`, so they read and wrote the owner's knowledge base
(`~/.local/share/tracemaker/kb.sqlite`, rows about id 219-232, board hash `db5fe713ba3c1a19`). That is why they seemed to differ
(128/52/140 routed). The rows are still there; delete them if they should not count.

**Consequence for tonight:** the first GPU-path runs of phases 1 and 2 (`soak-det-a`, `soak-det-b`, `soak-opt-relax`) crawled
(boards at 0.3-1 core, many stopped by the 3,600 s safety limit before their work budget ran out, so not comparable). I stopped them at
01:31 UTC and restarted every remaining phase with `--no-gpu` (`bench/results/soak-1025/queue.sh`, runs `soak-c-*`). The partial
logs remain in `bench/results/soak-1025/{det-a,det-b,opt-relax}.log`. Phase 2 changed too: `--txn` is not a `route` option and the
bundle-escape / supply options were proposals only, so phase 2 is `--relax`, `--soft-zones --si-rules` (KiCad refills) and `--escape-flow`.

### Phase 1: determinism, CPU fields (`soak-c-det8`, 3 M work, 3 x 8 threads)

All 33 boards in **3 minutes** (route CPU 1,836 s), no board gained a KiCad error. Against the stopped GPU-path run `soak-det-a`
(same options, GPU fallback): **24 of 25 boards it finished are byte-identical**, though they took 10-3,650 s there and 3-18 s here
(e.g. ErgoDone 1,708 s vs 5.9 s, Aleste 3,396 s vs 7.9 s). The exception, `kitspace_d20_tri_r0.3`, hit the 3,600 s safety stop on
the GPU path (119 routed vs 292), as a time stop should. This supports the phase 6b finding at board-set scale: identical output, with
a 10-300x slowdown whenever the GPUs are full.

**Phase 1b (`soak-c-det4`, 6 x 4 threads): 33/33 boards byte-identical to 3 x 8 threads** (`compare-det8-det4.txt`); route CPU time
1,836 vs 1,839 s. `compare_runs.py` prints "Gates: FAIL" only because KiCad's DRC list is capped on 7 large boards (ErgoDone,
sd_wav, LeeChee, PCIE-to-MXM, ...), so the gate there is "unknown", not failed; 0 boards with added errors in either run.

### Phase 2a: `--relax` is round-based, so `--work` does not bound it (`soak-c-relax`, stopped)

`--relax` implies `--round-work 20000000 --rounds 200` (doc 05, ramp), so `--work 3000000` does not limit it: each board runs until
the board is complete, the rounds stall, or the 3,600 s safety stop. On CPU fields serial_gw finished **172/172, KiCad-clean**, in
838 s (0 errors added); E3 and EtherCAT were still climbing after 25 min at about 6 cores each. 33 boards would take about 11 hours,
so I stopped it at 02:02 and queued `--relax` on the 9 smallest boards with a 20-minute cap (`soak-c-relax9`, after queue 2). Not a
bug, but worth a sentence in the `--relax` help: "--work does not apply; use --round-work/--rounds or --time".

### Phase 2b: `--soft-zones --si-rules`, KiCad refills (`soak-c-softsi`, 3 M work, 3 x 8)

33 boards in 5 minutes, all exit 3 (not all routed), **0 boards with added KiCad errors**. Router-routed connections 13,593 of 16,935
(vs 14,662 without soft zones: plane nets are left to the pour).

### Phase 2c: `--escape-flow` (experimental escape plan v2), 3 M work, 3 x 8 (`soak-c-eflow`)

No crash, **0 boards with added errors**, 20 of 33 boards differ from the default and routed connections go 14,662 → 14,740
(completion 82.5 → 82.9 %), same CPU time. A small gain on these boards for an option marked experimental.

### Phase 5: srj20 BGA boards a single variant leaves open, `--relax` (`soak-c-srj20relax`, 6 x 4 threads, stopped)

**12 of 12 finished boards routed completely and KiCad-clean** (sample006, 007, 012, 013, 014, 020, 024, 030, 031, 037,
039, 045: 46-78 connections, 185-1,856 s each, most under 4 min), 0 errors added. The next six (sample008, 016, 032, 040, 048, 054) were still climbing after 15-53 min at about
4 cores each: relax runs those until the board is complete or the 3,600 s cap. With 61 boards left that would not finish tonight, so I
stopped the phase at 03:03 to run the seeds, the 30 M determinism check and the sanitizers. The full srj20 set (900 s cap) is last in
the queue.

### Phase 7: router seeds 2-4 on the tricky boards (`soak-c-seed{2,3,4}`, 3 M work, 3 x 8)

No crash, **0 boards with added errors** in any seed. Every board changes with the seed (0/33 byte-identical to the default seed),
as it should, and the result barely moves: routed connections 14,641 / 14,662 / 14,666 against 14,662 (mean completion 82.3-82.5 %).
No seed makes any of the 33 boards clean: what keeps them open is not luck in the order.

### Phase 1c: ten times the work (`soak-c-w30t8`, 30 M work, 3 x 8 threads, CPU fields)

All 33 boards finished within their work budget (longest 722 s wall, far under the 3,600 s cap), 0 boards with added errors. Mean
completion **82.5 → 90.9 %** (route CPU 1,836 → 17,846 s). Biggest gains: Aleste 1,075 → 1,365 of 1,616, mini_mass_prog_rig
860 → 983 of 995, d20_tri 292 → 410 of 553, poncho fpga-dongle 292 → 367 of 413. Still 0 clean: several stop 2-30 short (ErgoDone
354/356, LimeSDR 811/814, prog_rig 983/995, dorkyboard 821/830).

**Phase 1d (`soak-c-w30t4`, 30 M work, 6 x 4 threads): 33/33 boards byte-identical to 3 x 8 threads** (`compare-w30.txt`), route
CPU 17,846 vs 17,926 s, 0 added errors. Determinism holds at ten times the budget.

### Phase 3b: plane tier with `--soft-zones --si-rules`, 3 M work, CPU fields (`soak-c-planes`)

8 boards in one minute. The two known additions again: **interf_u `starved_thermal` +6** (reproduces the phase 3a finding at the
same count as `i8`, `m16-*` and `i24j`, now in 15.8 s instead of minutes) and StickHub `solder_mask_bridge` +14 (judge noise).
Reproduce: `bench/run.py --fixtures build/demos --set planes --work 3000000 --refill --route-args="--soft-zones --si-rules --no-gpu"`.

### Phase 3c: ASan on all 33 tricky boards, `--escape-flow`, 1 M work, 6 x 4 threads (`soak-c-asan-bd`, `detect_leaks=1`)

**No AddressSanitizer or LeakSanitizer report**: all 33 exit 3 (not all routed; ASan would give 1, LeakSanitizer 23), 0 boards with
added errors, 25 minutes. (`--relax` was not run under ASan: it ignores `--work`, see phase 2a.)

### Phase 4b: TSan on all 33 tricky boards, 300 k work, 3 x 8 threads (`soak-c-tsan-bd`, `exitcode=66`)

**No data race reported**: all 33 exit 3 (a report would give 66), 0 boards with added errors, 23 minutes.

### Phase 2d: `--relax` on 9 small tricky boards, 20-minute cap (`soak-c-relax9`, in progress at 05:19)

First three hit the cap with 0 added errors: MicroMaple 108/111 (98 without relax), memsarray 9/10 (9), fifogfx c64cart 64/104 (50).

Update 05:49: 8 of 9 done, 0 added errors on all. **Two tricky boards become KiCad-clean with `--relax`**: multisensor_cr2032
205/205 (204 without, 146 s) and ardfpga 214/214 (177 without, 153 s). Others at the cap: teensy-fx 195/196 (163), OtterPill
126/137 (122), PCIE-to-MXM 158/188 (150). With serial_gw (phase 2a) that is 3 of the 33 "not clean" boards cleaned by `--relax`.
Done 05:51: EtherCAT 254/272 at the cap (228 without, 244 at 30 M work). 9/9 boards, **0 added errors**.

### Phase 5b: full srj20 set with `--relax`, 15-minute cap, 6 x 4 threads (`soak-c-srj20`, in progress)

At 06:21: 40 of 205 boards done, **38 complete and KiCad-clean**, 0 boards with added errors.
At 06:51: 76 of 205 done, 68 clean, 0 added errors.
At 07:21: 114 of 205 done, 98 clean, 0 added errors.
At 07:51: 145 of 205 done, 122 clean, 0 added errors.
At 08:21: 174 of 205 done, 143 clean, 0 added errors.
Done 08:54: **163 of 200 boards complete and KiCad-clean** (exit 0), 37 not all routed; 8,385 of 8,715 connections (96.2 %);
0 boards with added errors; no crash.

### Queue 3: determinism of the non-default paths (from 08:55)

- **`--escape-flow`, 3 M work: 33/33 boards byte-identical** between 3 x 8 (`soak-c-eflow`) and 6 x 4 threads (`soak-c-eflow4`).
- **`--relax` with a fixed budget (`--round-work 2000000 --rounds 6`), 9 small tricky boards: 9/9 byte-identical** between 3 x 8
  (`soak-c-relaxr8`) and 6 x 4 threads (`soak-c-relaxr4`). So the ramp, legaliser and stall-restart are deterministic when bounded
  by work; only the wall-time stop made earlier relax runs unrepeatable.
- **`--relax` with the same fixed budget on the srj20 boards a single variant leaves open: 71/71 byte-identical** between 3 x 8
  (`soak-c-srjr8`) and 6 x 4 threads (`soak-c-srjr4`), 7 complete, 0 added errors, route CPU 18,444 vs 18,224 s.
- **`--soft-zones --si-rules`, 3 M work: 33/33 boards byte-identical** between 3 x 8 (`soak-c-softsi`) and 6 x 4 threads (`soak-c-softsi4`).
- **Plane tier, soft zones + SI: 8/8 boards byte-identical** between 3 x 8 (`soak-c-planes`) and 6 x 4 threads (`soak-c-planes4`);
  interf_u's `starved_thermal` +6 is therefore the same board each time, not chance.
  StickHub: byte-identical board, KiCad counts `solder_mask_bridge` 14 in one run and 15 in the other, which confirms that count is judge noise.

### 100 M work (`soak-c-w100`, stopped at the owner's word, 10:0x UTC)

25 of 33 boards done: +155 connections over 30 M work, still 0 clean, 0 added errors. More budget stops paying on these boards.

Testing stopped by the owner at about 10:05 UTC. No code changed, nothing pushed.
