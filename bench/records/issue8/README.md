# Issue #8 records: the viewer freezes on a routing stall

Design and protocol: [docs/09-visualization.md](../../../docs/09-visualization.md) section 8 (freeze), [docs/13-viewer-protocol.md](../../../docs/13-viewer-protocol.md) ("The `failure` verdict"),
decision D113. Base: the M16 branch (`m16-advisor`, PR #33), which this work builds on (it changes the same `failure` event).

## What was checked (`verification/`)

| File | What |
|---|---|
| `ctest-release.txt` | the full suite, release preset: 297 of 297 pass (one skip: `fixture_preparation_native`, needs KiCad's Python in Docker); the new tests: 5 router cases (`tests/test_stall.cpp`), the message builder and snapshot checks in `tm_server_tests`, and `viewer_stall` |
| `viewer-node-tests.txt` | `viewer/src/stall.test.ts` and `stall_draw.test.ts` under Node (17 tests, no browser) |
| `quick-compare.txt`, `planes-compare.txt`, `planes-soft-compare.txt`, `mid-compare.txt` | options off: routed boards byte-identical to `origin/main` (30/30, 8/8, 8/8, 11/11). `mid` prints `Gates: FAIL` for the reason in `bench/records/m16/README.md` (the capped-report judge of PR #28), and its CPU seconds differ because other jobs shared the machine: see the timing below |
| `overhead-and-identity.txt` | no slowdown without a viewer (17.3 s before, 17.2 s after, three runs each, alternating, idle machine) and a run with `--record` gives the same board bytes as a plain run |
| `failure-verdicts-sample104.txt` | the new `failure` fields on a real ball-grid breakout: 17 failures, 16 `contested` (fixed copper leaves the full 0.12 mm class width, the routed copper of other nets closes the way: the picture this repository's plateau work found) and 1 `unknown` |

## Looked at in a browser (`e2e/`)

Headless Chromium (Playwright's headless shell, SwiftShader WebGL2) against a live `tracemaker route ... --view --hold` on `sample104`, with
`#stall=3` in the URL: `e2e-stall-live.mjs`. The viewer latched on the first stall (3 connections listed, then 14 as more failed), the stalled net stayed highlighted,
`]` moved to the next connection, the label and verdict were drawn beside the closest approach, and Space resumed the stream (no console errors).
`live-latched.png` and `live-next-connection.png` are screenshots of that run. A run that has already finished does not latch when a viewer joins late (tested in
`stall.test.ts`); an earlier screenshot on a one-connection fixture showed the label covering the rejected path, which is why the label now goes above or below the anchor.

## Not verified

- The label's wording and colours were judged on those screenshots only; nobody has used the freeze on a real stall of their own board.
- Pocket failures (a pin boxed in at its source) carry no rejected path (the search never left the pad); they get the verdict and numbers only.
