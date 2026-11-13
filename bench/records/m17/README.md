# M17 records: installable and measured (docs/10 section 11, docs/12 D121 and D122)

2026-10-16, branch `m17-installable` (main `d685629` plus the commits of this change). Machine: 56 cores, no GPU used (the release binary is CPU only). Judge: `kicad-cli` 10.0.6
(Docker `kicad/kicad:10.0.6`), errors added against the input plus connectivity, as `bench/run.py` always judges. Binary for every run below: the `dist` preset's
`tracemaker` (static libstdc++, SQLite and zstd), i.e. what the release carries.

## 1. Release files (A2)

Not a table: the checks and what each showed.

| check | result |
|---|---|
| `scripts/make_release.py --build-viewer` in a clean `ubuntu:26.04` container with only the packages the workflow installs (simulating its `package` job) | tarball 8.5 MB, plugin zip 6.9 MB (with the placer), about 3 min; glibc floor of the binary 2.38 (`fmod`, `strtol` family) |
| the binary's shared libraries | `libm`, `libc` only |
| `scripts/test_release_install.py` in `ubuntu:24.04` (no compiler, no cmake, no Python): version, viewer page from `../share/tracemaker/viewer`, route `plane_smd` (`--soft-zones --via-in-pad --keep-vias-off-pads --first-nets XIN,XOUT --work 3000000`), `drc --refill-zones`, validate the zip | pass; 49 of 49 connections, 0 DRC errors, 0 unconnected; 5 s from unpacking to checked result (6 s with the container start) |
| the same on `debian:13`, `fedora:latest` | pass (6 s, 5 s inside) |
| the same on `debian:12` | **fails**: `version 'GLIBC_2.38' not found` (glibc 2.36). Ubuntu 22.04 (2.35) was not run |
| the tarball and the zip built twice from one build | identical `SHA256SUMS` |
| plugin zip unpacked by Python's `zipfile` (no executable bit kept), no `TRACEMAKER` variable, run in the live KiCad 10.0.6 GUI by `scripts/kicad_gui_roundtrip.py --package` (`gui-package.log`) | the plugin found `plugins/bin/tracemaker`, added 498 tracks and 15 vias; one Ctrl+Z removes all, redo brings them back; KiCad's DRC: unconnected 148 to 28, no new error |

GitHub: `.github/workflows/release.yml` ran on the pull request (#44): package job 4 min 26 s, install job 17 s, both green; the publish job is skipped off a tag, so uploading to a draft release had not run at the time (it ran on the `v0.10.0` tag on 2026-10-18: package, install and publish all green, draft release with the two files and `SHA256SUMS`).

Not checked: the publish step on a tag, KiCad's own *Install from File* on this archive and the virtual environment it builds for
kicad-python, macOS arm64 (not built), Ubuntu 22.04.

## 2. PCBWorld-Bench D3 (A1)

Run: `bench/run.py --set pcbworld-d3-test --time 120 --jobs 3 --threads 8 --name m17-pcbworld-d3`; Freerouting 2.1.0: `bench/pcbworld_fr.py --name m17-pcbworld-fr21 --jobs 8 --runs 3`
(jar sha256 `2c07d58f75dac03782664081e7a58b41c25400d871a9fcf166a2ea6fe60d5def`). Tables from `bench/pcbworld_d3.py m17-pcbworld-d3 --fr m17-pcbworld-fr21`:

# PCBWorld-Bench D3 test boards, run `m17-pcbworld-d3`

| class | boards | clean | clean % | mean routability | boards open | median s | Freerouting RC12 clean | TraceMaker clean, FR not | FR clean, TraceMaker not |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| D3-A | 99 | 97 | 98.0 | 0.994 | 2 | 1.3 | 94 (94.9 %) | 4 | 1 |
| D3-B | 10 | 10 | 100.0 | 1.000 | 0 | 10.0 | 9 (90.0 %) | 1 | 0 |
| D3-C | 10 | 9 | 90.0 | 0.996 | 1 | 61.4 | 8 (80.0 %) | 1 | 0 |

Freerouting 2.1.0 with the paper's settings (`-mp 10`, 4 GiB), 3 runs per board, judged by the same judge as TraceMaker above:

| class | boards | clean, mean of runs (@1) | clean, any of 3 runs (best of 3) | mean routability | median s per run | TraceMaker clean, FR best-of-3 not | FR best-of-3 clean, TraceMaker not |
|---|---:|---:|---:|---:|---:|---:|---:|
| D3-A | 99 | 0.670 | 73 (73.7 %) | 0.978 | 4.1 | 24 | 0 |
| D3-B | 10 | 0.500 | 5 (50.0 %) | 0.992 | 7.0 | 5 | 0 |
| D3-C | 10 | 0.200 | 3 (30.0 %) | 0.758 | 66.3 | 6 | 0 |

Published (arXiv 2607.05915, Table 3; best of five rollouts, Freerouting and PPO the mean of four seeds; KiCad 9 and the paper's own rule files):

| method | D3-A CP | D3-A Rout. | D3-B CP | D3-B Rout. |
|---|---:|---:|---:|---:|
| Reference (the human layout) | 1.00 | 1.00 | 1.00 | 1.00 |
| Freerouting 2.1.0 | 0.80 | 0.91 | 0.78 | 1.00 |
| KiCadRoutingTools | 0.74 | 0.94 | 0.20 | 0.86 |
| OrthoRoute | 0.02 | 0.53 | 0.00 | 0.44 |
| GPT-5.4 (agent) | 0.65 | 0.91 | 0.00 | 0.62 |
| PPO (trained on synthetic boards) | 0.86 | 0.95 | 0.45 | 0.85 |

Boards TraceMaker loses (the nightly Freerouting 2.5.0-RC12 flag from PCBench's data, or a clean run of Freerouting 2.1.0 here):

- D3-A `AVR-ISP_pogo-plug_1.27mm_AVR-ISP_pogo-plug_1.27mm`: routed 3/6, added errors none, 2 s

Details, the correction of doc 18 (D3 is derived from PCBench) and why the published rows are not comparable: docs/10 section 11.1 and 11.2.

## 3. `--txn` and `--relax` on the 33 failing tier B-D boards (C1)

The 33 boards (`bench/sets/pcbench-bd-fail.txt`) are the boards of tiers B, C and D that the 0.9.0 run (120 s, 8 variants) did not finish clean: 11 + 12 + 10, all with
connections left open and none with an added error. 16,935 connections, 1,646 of them open in the single-variant base arm.

**One variant, 100 M work, one thread per board** (`c1.sh`, `c1b.sh`; `bench/run.py --set pcbench-bd-fail --work 100000000 --jobs 12 --threads 1 --route-args "--variants 1 --no-gpu ..."`):

| arm | boards | connections | routed | open | more than m17-c1-base | fewer | clean boards | errors added | CPU s |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| m17-c1-base | 33 | 16935 | 15289 | 1646 | 0 | 0 | 0 | 0 | 4858 |
| m17-c1-txn | 33 | 16935 | 15320 | 1615 | 7 | 0 | 0 | 0 | 3924 |
| m17-c1-relax1 | 33 | 16935 | 14206 | 2729 | 3 | 28 | 0 | 0 | 4644 |
**Default eight-variant portfolio, 100 M work per variant (the relax arm: 5 rounds of 20 M), 8 threads per board** (`c1c.sh`, `c1e.sh`; the `base8` line in `c1c.sh` failed on an
argument and was rerun by `c1e.sh`):

| arm | boards | connections | routed | open | more than m17-c1-base8 | fewer | clean boards | errors added | CPU s |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| m17-c1-base8 | 33 | 16935 | 16105 | 830 | 0 | 0 | 0 | 0 | 77014 |
| m17-c1-txn8 | 33 | 16935 | 16223 | 712 | 15 | 4 | 0 | 0 | 96039 |
| m17-c1-relax8 | 33 | 16935 | 15596 | 1339 | 11 | 16 | 2 | 0 | 106470 |
Per board, `--relax` in the portfolio against the base: better on 11, worse on 16, equal on 6; it completes
`kitspace_ardfpga` (214 of 214) and `Hardware_Playground_multisensor_cr2032` (205 of 205) with no error added, the only clean boards in any arm. Taking the better of base and relax per
board would route 16148 connections against the base's 16105 (a post-hoc choice, with both arms' compute).

Caveats. (1) The work budget is equal, the CPU time is not: the CPU column shows `--txn 1` at +25 % and `--relax` at +38 % of the base in the portfolio arms (one variant: `--txn` -19 %, relax -4 %,
which is run-to-run and load noise as much as anything, the machine was shared). (2) `mini_mass_prog_bench_prog_rig` reached `bench/run.py`'s one-hour safety stop in the `txn8` (3,666 s) and
`relax8` (3,819 s) arms, so those two results depend on wall time; the base arm finished in time. (3) One seed, one budget, 33 boards chosen because the default leaves them open: no statement about boards
the default completes. (4) `--relax` with `--variants 1` was silently ignored until this change: `c1-relax-ignored.md` is that arm, identical to the base on every board (`m17-c1-relax`).
(5) Both options are hidden or off by default on these boards; `--txn` is automatic on breakout boards only.

## 4. Quick tier

`bench/run.py --set quick --work 1000000 --route-args=--no-gpu --jobs 4` with the release binary against `bench/results/m18-quick-new` (main's engine): routed boards byte-identical 30/30
(`quick-identity.md`). The only change to routing code is that the round-based options now reach the portfolio when `--variants 1` is given.

## 5. `--txn 1` at equal wall time on tiers A-D (2026-11-13)

Branch `roadmap-2026-11-13` (engine of `ed0ca60`; the only routing change on the branch since main is behind `--plane-informed`, which is off).
Script `txn-tiers.sh`: both arms run at once with the same jobs, `bench/run.py --tier X --limit 40/40/30/40 --time 120 --threads 8 --jobs 3`, the second
with `--route-args=--txn 1`. Machine shared by both arms and their Freerouting reference runs (load about 20 to 50); GPU an RTX 5060 Ti.

| tier | boards run | judged in both arms | clean, base → `--txn 1` | routed on those boards | harness timeouts, base / `--txn 1` |
|---|---:|---:|---:|---:|---:|
| A | 40 | 40 | 40 → 40 | 2,637 → 2,637 / 2,637 | 0 / 0 |
| B | 40 | 33 | 30 → 30 | 3,972 → 3,987 / 4,029 | 4 / 5 |
| C | 30 | 20 | 16 → 16 | 5,981 → 5,995 / 6,025 | 7 / 9 |
| D | 22 | 15 | 9 → 8 | 10,035 → 10,040 / 10,246 | 6 / 6 |
| all | 132 | 108 | **95 → 94** | 22,625 → 22,659 / 22,937 | 17 / 20 |

"Judged in both arms": both have a KiCad-judged result. A harness timeout is the engine still running at 3 × 120 s + 60 s; the base has 17, so the `--time` stop
itself overruns on these boards under this load (both arms equally exposed).

- Lost: `m2-electronics_m2fc` (clean → 723/724, 120 s → 412 s). Gained clean: none.
- Largest gains, all on boards that stay open: `poncho_fpga_fpga-dongle` 389 → 404 / 413, `zx-sizif-512-ext_sizif512ext` 454 → 468 / 498,
  `Aleste-520EX_aleste` 1,426 → 1,434 / 1,616.
- Timed out only with `--txn 1`: EtherCAT_shield_v1, serial_gw_ATMEGA328P (clean in the base), kitspace_ardfpga, ErgoDone, P8000_WDC_Emulator,
  mechkeys_58r-mxalps (clean in the base), LeeChee_1800, mechkeys_LFK78. Only in the base: fifogfx_c64cart, kitspace_OtterPill,
  CurrentSensorPCB_line, Teensy_test_layout, mini_mass_prog_bench_prog_rig.
- Tier A: 40/40 clean in both, 33/40 byte-identical, route CPU 3,073 → 2,896 s.

Decision D181: `--txn 1` is not made a default variant. Per-tier tables from `bench/compare_runs.py`: `txn-tier{A,B,C,D}-compare.md`; runs in
`runs/m17-txn-{base,txn}-tier{A,B,C,D}/` (`boards.jsonl`, `summary.json`, `run.log`).

## Files

`runs/<run>/boards.jsonl` and `summary.json` of every run named above, the three tables (`pcbworld-d3.md`, `c1-single-variant.md`, `c1-portfolio8.md`), `gui-package.log`, the shell scripts of the C1
arms, `quick-identity.md`.
