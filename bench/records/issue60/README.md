# Issue #60: tuned nets stay tuned (D173)

Doc 05 §73, decision D173. `delay_room` failed on main after D154: the router's log said `/Debugger/D+` was tuned to 78.44 ps, but
the written board measured 70.56 ps against D-'s 79.25 ps (0.5 ps skew rule).

## Diagnosis

- `diagnosis-stages.txt`: the job's result after each stage (router, refill repair, thermal repair, after-route passes). The pair's
  copper never changes after the router, so no later stage was at fault.
- `diagnosis-variant1.txt`: the winning variant alone. Its search ends with 128 routed against a best of 129, so the clean-up (and the
  tuners in it) is skipped, and the best state goes out untuned. The tuning lines in the full log come from other variants.

## Runs

The B2 time-domain fixtures (`bench/records/m21-b2/make_fixtures.py` from `build/demos`), as in `bench/records/d152-room`. Both runs
used the same `bench/run.py` checkout and set the binary with `TM_BINARY`:

```
TM_BINARY=<binary> bench/run.py --fixtures build/i60fix --boards RoyalBlue54L-Feather-delay RoyalBlue54L-Feather-profile \
  interf_u-delay interf_u-profile --work 3000000 --refill --route-args=--soft-zones --threads 8 --jobs 2 --set-name b2-delay \
  --name issue60-b2-<base|new>
```

| Run | Binary |
|---|---|
| `runs/issue60-b2-base` | main (057ee82) |
| `runs/issue60-b2-new` | this change (D173) |

Judged by KiCad 10.0.6 after a refill:

| Board | main: routed, unconnected, added errors | D173: routed, unconnected, added errors | Output |
|---|---|---|---|
| RoyalBlue54L-Feather-delay | 129/147, 37, `skew_out_of_range` 1 | **129/147, 39, none** | differs |
| RoyalBlue54L-Feather-profile | 129/147, 37, none | 129/147, 37, none | identical |
| interf_u-delay | 152/164, 17, `starved_thermal` 4 | 152/164, 17, `starved_thermal` 4 | identical |
| interf_u-profile | 152/164, 17, `starved_thermal` 4 | 152/164, 17, `starved_thermal` 4 | identical |

On RoyalBlue54L-Feather-delay, the best state is now cleaned up and tuned. D+ ends at 79.43 ps against D-'s 79.49 ps. The copper is
3.8 mm shorter (smoothing ran) with the same 94 vias. The meanders and the re-routed connection cut the soft fills once more: the
refill counts 41 unconnected items against 40, the refill repair closes 1 of them against 2, and KiCad counts 39 unconnected items
against 37. Those are two more open zone connections, with the same routed count. That is the price of meeting the skew rule on this
board; the run without tuning met the routed count by leaving the rule broken.

interf_u-delay has delay rules, but its search ended in its best state, so nothing changed. The two profile boards have no length
or skew rule.

**Boards without length or skew rules** are unchanged by construction (both parts are gated on such rules). None of the 30 quick-tier
boards has a `.kicad_dru`. Two of them were routed with both binaries at 1 M work and came out byte-identical:
`655_testboard` (57/57) and `C-BISCUIT_buck-reg-5v` (55/55). The quick tier was not run.

## Tests

- `delay_room`: passes (129/147, no skew violation, `room for /Debugger/D+: 2 ripped, kept`). On main it failed with 1 skew
  violation.
- `length faults: a stage that shortens a tuned net is worse, one that leaves it is not (D173)` (unit, `tests/test_length.cpp`).
- Also passed: `length_tuning`, `delay_tuning`, `diff_pair`, `bus_route`, `after_route_passes`, `crules_route`,
  `thread_determinism` and `late_reach_verify`.
