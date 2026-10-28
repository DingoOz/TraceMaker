# Morning report, 2026-10-28: roadmap night

Asked on 2026-10-27 at about 22:10 UTC: "work on the roadmap features without stopping until I tell you". Everything is on the local
branch `m20-c5-fanout` in a separate worktree, from `origin/main` `ec7cc9b` (PR #51). Nothing is
pushed. The choices I made without asking are in `dev/assumptions.md` (A71 to A93).

## What was done

| Item | Result | Decision | Records |
|---|---|---|---|
| M20 C2, pin swap | An oracle (minimum-cost assignment of connector pins and resistor arrays) on 30 boards: open 982 → 978 and 27 → 26, median gain zero. **The pass is not built**; C2 is closed | D141 | `bench/records/m20-c2` |
| M19, KiCad drift | `scripts/kicad_drift.py` and a weekly workflow against the newest `kicad/kicad` release and release candidate; 4/4 boards pass on 10.0.6, 10.0.7 and `nightly` (a stale 10.0 rc). Found: `route -o` did not copy `.kicad_pro`/`.kicad_dru` beside the output, so KiCad judged it with default rules; fixed | D142 | `bench/records/m19-drift` |
| M20 D3, run report | interf_u (128 s, 140/164) and RoyalBlue54L-Feather (72 s, 102/147, not time-limited) now finish; all eight plane boards have reports. Still missing: a review by someone who did not write it | — | `bench/records/m20-run-report` |
| M20 C5, failures by cause | `tracemaker causes`: of 1,320 open connections on the 33 failing boards, 779 are blocked by copper near their pins. Strict searches now start from the more crowded end, in four of the eight portfolio variants: open 1,383 → 1,220 on the failing boards, 69 → 46 on the 59 clean ones (clean 44 → 46), mid tier 1,306 → 1,093, quick tier 32 → 24. No board gained an error | D143 | `bench/records/m20-c5` |
| M21 B3, audit loop | `--si-rules` stitches the critical nets' unstitched layer changes (`--no-si-stitch` turns it off), and the report names each change left and why. On the plane tier there was nothing to stitch: one change on StickHub is left, with no plane copper on one side | D144 | `bench/records/m21-b3` |
| M21 A5, settle `--si-rules` | Five seeds on the plane tier: no critical-net measure moves with the seed. `--critical-first` routes 27 of 28 critical nets against 23 to 25 without it. `--si-rules` keeps all three rules, and defaults stay off | D146 | `bench/records/m21-a5` |
| M21 B1, KiCad 10 tuning profiles | Profiles are read, KiCad's implicit width and gap rules are compiled as 10.0.6 applies them, single-ended widths are narrowed per layer, and pairs stay on one geometry. **The first result was wrong**: see "B1 rejudged" below | D145 | `bench/records/m21-b1` |
| M21 B2, time-domain matching | Delays are measured as KiCad 10.0.6 measures them (its QA board to 0.01 ps; 325 of 329 nets on four demo boards to under 1 µm). ps windows and skew groups are tuned. ddr8 goes from 20 errors to 0, interf_u from 7 to 0 and RoyalBlue from 1 to 0, by KiCad's DRC and at the same routed counts. KiCad's diff-pair coupling test is now in the DRC (parity corpus 229/229), and the router avoids coupling-gap errors (RoyalBlue's 5 → 0) | D147, D148 | `bench/records/m21-b2` |

## Things to know

- **B1 rejudged.** B1's fixtures kept the demos' net settings at schema 4. KiCad migrates those and drops every class's tuning profile,
  so B1's "0 added KiCad errors" came from a KiCad that checked nothing. At schema 5 the first build had 20 to 175 `track_width` errors
  per board. Three fixes (KiCad semantics):
  - classes with no profile take Default's;
  - a single-ended profile sets the pair width;
  - profiled nets get no neck-down.

  With them, plane_smd is 48/49 with no added error. RoyalBlue drops to 102/147 (single-ended profile) and 104/147 (both), from 125, with
  no width errors. So a single-ended profile on Default is not free: KiCad wants exactly its width on every Default track. D145 and
  doc 05 §58 say so.
- **The 5 coupling-gap errors on RoyalBlue are fixed** (D148, after B2). RoyalBlue with the USB pair profile had them at the USB-C
  connector. Its D+/D− pads are interleaved at 0.5 mm, so the two halves leave in parallel 0.35 mm apart, and a profile's gap must be
  exactly 0.15 mm wherever the halves see each other. The router now runs KiCad's coupling test in the exact check and on each search
  step for pair nets under a bounded gap rule. Result: 104/147 as before, with no gap errors. The other seven B1/B2 fixture boards are
  identical. Testing at commit only was not enough: it lost 3 connections, because the search kept finding paths the check refused.
- **Pair names as KiCad reads them** (D149). `DQS_P0`/`DQS_N0` are now a pair, as `MatchDpSuffix` makes them, and `inDiffPair('D')`
  matches `D_P`. The rule-parity corpus has 231 cases, all matching KiCad. Of 4,143 fixture boards only vme-wren and one freerouting
  fixture pair differently.
- **Vias that remove unconnected copper** (D150). KiCad drops a via's or pad's ring on layers nothing connects to
  (`remove_unused_layers`); the engine judged and filled around those rings. It now flashes them as KiCad does, splits the item
  into its copper and a bare hole, and refills as KiCad's filler does. On vme-wren the engine's DRC went from 22,362 clearance and
  21 shorting reports (KiCad: none) to 425 and 20, which have other causes. Two more KiCad coupling rules came out of the same
  comparison: tracks of a tuning pattern are left out, and tracks a few nanometres off parallel are coupled (SEG::Intersect gives up
  beyond 32 bits). vme-wren's 28 gap and 6 uncoupled-length errors are now KiCad's exactly. Only four fixture boards use the option,
  none in a bench tier; the router treats it more strictly than KiCad (A92). Records in `bench/records/d150-padstacks`.
- **Clearances in KiCad's order** (D151). A custom clearance rule is final in KiCad, even below the board minimum, and a pad's local
  override beats custom rules; the engine had it the other way round on both counts. Two shapes of one footprint are never tested
  against each other in KiCad. With this, vme-wren matches KiCad on all 15 routing-relevant types (it had 22,362 clearance reports
  before D150). The other demo boards are unchanged. **When merging with D139**, its per-class cache check must use this order.
  Records in `bench/records/d151-clearance`.
- **D139 is not on this branch.** On `m20-c5-fanout`, any custom rule other than a disallow (a skew rule, say) switches the router to
  exact checks and turns the distance fields off: RoyalBlue routes 119 with B2's rule file and 125 without it. D139
  (`cache-safe-custom-rules`) fixes that, and merging the two is yours.
- **LAN addresses in route summaries.** `route --json` recorded `--view-embed`, which lists the machine's LAN, VPN and host addresses.
  The engine now records it as "(not recorded)" like the token. The B1 records committed earlier tonight had them, so I scrubbed them
  and amended that local commit (17b2178 → 69bb1af; nothing was pushed). Check before pushing anything older.
- **`tracemaker drc` is slow on the Jetson board** after the checks finish, in the old binary too (over 5 minutes, CPU bound, probably
  `starved_thermals`, which runs after the DRC). `kicad_drc_parity` still passes within its timeout. Not looked into.
- **Not done:**
  - `fromTo()` in rules (vme-wren's length rules are unchecked);
  - ps skew in the bus tuner;
  - a net in two classes takes its parameters from the first;
  - A3 (the Kookabus files are still missing).
- From earlier in the night:
  - multichannel_mixer gains one `starved_thermal` with the crowded variants under `--soft-zones` (D143, "found afterwards");
  - the `plane_smd` test now uses four variants (A84);
  - the D3 run report still needs a review by someone who did not write it;
  - the progress website serves the main checkout, not this worktree;
  - direct `route` runs without `--no-kb` read the knowledge base, so two runs can differ.

## Tests

- Full `ctest` on the B2 build (6e9b774): 415 of 415 passed (1 skipped, `fixture_preparation_native`).
- Quick tier, run once at the end (`b2-quick`): identical to `m21-quick` (17b2178) on all 30 boards in routed counts, tracks, vias and
  added errors; clean 0.8667, completion 0.9931.
- Rule parity corpus: 229/229 against KiCad 10.0.6.
- After D148 (coupling-gap avoidance): full `ctest` 416 of 416 passed (1 skipped), with the new `[coupling][route]` case. The eight
  B1/B2 fixture boards were rerun (`m21-b2/runs/couple-b1`, `couple-b2`). The quick tier was not rerun: none of its 30 boards has a
  `diff_pair_gap` rule or a tuning profile, so the new check never runs on them.
- After D150: rule parity 234/234 (three new cases, which the D149 build fails); vme-wren pair verdicts identical to KiCad's; jetson
  still matches; the eight B1/B2 pair-rule boards route byte for byte as with D148 (`d150-padstacks/runs`). The quick tier was not
  rerun: none of its boards has a pair rule, a tuning pattern or a via that removes copper (checked). Full `ctest`: 420 of 420 passed (1 skipped).
- After D151: rule parity 239/239; vme-wren and jetson match KiCad; the other demo boards as before (microwave closer). Full `ctest`
  420 of 420 (1 skipped) before the last change (footprint graphics of two footprints); after it, the DRC, rules, refill and coupling
  unit tests (90 cases) and the DRC and rule-parity integration tests. The quick tier was not run: no bench board has a rule below
  its board minimum or a pad override next to a custom clearance rule (checked on all 76 rule files).

## Commits (local, branch `m20-c5-fanout`, nothing pushed)

- `64704bf`: C5 follow-ups (D143, A84)
- `69bb1af`: B3, B1 and A5 (D144-D146; amended to scrub the LAN addresses, previously 17b2178)
- `6e9b774`: B2 (D147)
- `b0e6dc7`: B2 quick-tier record and this report
- `82f81cf`: coupling-gap avoidance in the router (D148, A90)
- `c6d0f4b`: pair names as `MatchDpSuffix` reads them (D149, A91)
- `5b8b4ca`: vias and pads that remove unconnected copper, tuning patterns, nearly parallel pairs (D150, A92)
- clearances in KiCad's order, copper graphics, NPTH hole names (D151, A93)
