# Morning report, 2026-11-18

Worktree `TraceMaker-rm3`, branch `m26-impl` (on `m26-bga-fanout`, PR #92). Six local commits, **nothing pushed**:

| Commit | What |
|---|---|
| 1a3efce | M26 F8: a ramp from a warm start never returns fewer routes than the warm start (on) |
| b8d40ce | M26 F1-F3, F5 measured: via sites are lost to tracks; reservations and via-cost guidance lose to the control |
| 9b77616 | M26 F4, F6, F7, F9: package lanes and crossing order as options (off); crossings per connection in the route summary with a two-layer hint (on) |
| 6ebd450 | M25: explanation page "how the default route decides", one board through all 16 stages |
| 2637970 | Errata for the 2026-11-14 LaTeX routing report (6 wrong claims; PDF not rebuilt, no LaTeX here) |
| a65e0ba | M24 (D192): exact courtyards and Margin lines in the placer; `--closure-keep-overlaps` (off) |

## M26 — BGA boards that never complete (doc 18 §21, D191)

All nine items are built or measured.
- **Adopted:** F8 (warm-start floor) and F9 (`line_crossings` in the summary, plus a "next" hint on two-layer boards at ≥ 11.5
  crossings per connection; it flags 8 boards, all never-clean).
- **Not adopted:** F2-F7. Each left more connections open than the router's normal variant mix (control 7/7/6 open on boards
  152/070/040).
  - F4 kept 0 of 427 via moves.
  - F6 lanes: 8-16 open.
  - F7 crossing order: 10-13 open.
- **Checks:** with the options off, `quick` and `mid` are byte-identical.
- **Gate not met** (≥ 3 of 11 clean). Records are in `bench/records/m26`.
- **Fan-out first, then kept** (tried 2026-11-17 evening, `bench/records/m26/fanout`): 12-20 open against the control's 6-7. Not adopted.

## M25 — explanation page

`docs/explanation/default-route.md` is in the nav, and the run is recorded in `bench/records/m25`. Milestone done.

## M24 — move parts a hand placement has pinned down (D192, doc 04 §12)

**What changed in the placer:**
- **Exact courtyard outlines (on).** These remove only 1 conflict on the three blocked boards; the rest are real overlaps that
  KiCad also reports.
- **Margin layer lines (on).** Pads are now held off them, as KiCad does.
- **`--closure-keep-overlaps` (off).** A moved part may keep an input overlap if their courtyards really overlapped, copper and
  holes stay clear, and no new pair appears.
- **Bench judge:** a second placement gate (`place_error_gate_pairs`) matches footprint errors by footprint, not by position.

**Results** on the 14 closure boards: 32 open, 4 complete, 3 clean — the same totals as D190.
- **Gains:**
  - serial_gw is now clean.
  - phone_amp goes from 5 to 4 open.
- **No change:** memsarray. Under D190 it also tried 34 moves, so "no candidates" there meant exhausted, not empty.
- **Loss:** 58r-mxalps. A newly legal move came up early, and the time limit arrived before D190's completing move.
- **Bugs found and fixed:** a first version produced real KiCad errors — a short on phone_amp, and a new overlap on 1Bitsy. The
  three reruns are in `bench/records/m24`.
- **Gate not met**, so the keep rule stays off.
- **Validation:**
  - Placer tests: 38 cases pass. Judge tests: 21 pass.
  - The quick tier was not rerun, because these changes cannot reach its boards (it routes, it does not place).
  - Exact courtyards and Margin lines are now on for every placement mode. They were measured only on these 14 boards, so a
    placement benchmark is worth running before merging.

## Still open on the roadmap

- **M17:** macOS arm64 build. Needs the hardware.
- **M19:** KiCad 11. No release candidate exists yet.
- **M20 D3:** needs a human designer to review the reports.
- **M23 E5:** the throughput gain is below the noise at equal time.
- **M24 and M26:** gates not met (above).

**Decisions for you:**
- Push `m26-impl` (onto PR #92 or as a new PR)?
- Run a placement benchmark before the always-on courtyard and Margin changes merge?
