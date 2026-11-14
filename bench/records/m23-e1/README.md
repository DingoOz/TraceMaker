# M23 E1: power nets wide from the start (doc 05 §79, D184)

2026-11-13, branch `features-2026-11-13`. Machine: 56 cores, RTX 5060 Ti (fields on the GPU where they fit; results do not depend on it).
Judge: `kicad-cli` 10.0.6 (Docker), errors added against the input plus connectivity, as `bench/run.py` always judges.

- Set: `bench/sets/power-wide.txt`. 30 PCBench boards (seed 7) of the 314 under 800 kB whose designer drew at least half of the
  supply-net copper (≥ 100 mm) at 0.5 mm or wider. "Supply" is `route --power-width`'s test, mirrored in `bench/power_widths.py`.
- Run: `run.sh`, base and `--power-width 0.5` at 3 M work (deterministic, `--no-kb`), both at once.
- Experiment: `--power-width 0.5` with wide attempts in the first pass only, on the 9 boards the base leaves open
  (`runs/e1-pw05-strict`, `bench/sets/e1-open.txt`; a temporary switch, removed).

| arm | routed / 4,038 | clean | supply copper at ≥ 0.5 mm |
|---|---:|---:|---:|
| base | 3,946 | 21 | 20 % |
| `--power-width 0.5` | 3,944 (−2) | 21 | **94 %** |
| designer (`raw.kicad_pcb`) | – | – | 84 % |

- No error added on any board; CPU −2.9 % at the same work.
- Changed routed counts: ErgoDone +1, BTTF-TimeCircuits +2, SPEX_HAB −4, Unifying −1.
- First pass only (9 open boards): routed 2,178 as with every pass, width share 94 % → 91 %. Dropped.
- Per board: `power-widths.md` (share at ≥ 0.5 mm and median width for base, power width and the designer) and `power-widths.json`.
  `compare.md` is `bench/compare_runs.py e1-base e1-pw05`; its regression gate fails on SPEX_HAB and Unifying, as listed above.

Tests: `tests/test_power_width.cpp` (wide where it fits, class width where it does not, a narrower preference ignored) and
"neck at pads" in `tests/test_passes.cpp` (both ends, 45 degrees exact, off-angle left whole, floor). The quick tier was not run:
with no preferred widths the router's path is unchanged and the neck step does not run.
