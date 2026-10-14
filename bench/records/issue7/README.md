# Issue #7 records: reference-return audit of high-speed nets

Design: [docs/05-routing.md](../../../docs/05-routing.md) section 42, protocol [docs/13-viewer-protocol.md](../../../docs/13-viewer-protocol.md) (`return_path`), overlay
[docs/09-visualization.md](../../../docs/09-visualization.md), decision D114. Base: the issue-8 branch (PR #34), which this builds on (the viewer's side panel and the
Canvas2D overlay helpers).

## What was checked (`verification/`)

| File | What |
|---|---|
| `ctest-release.txt` | the full suite, release preset: 309 of 309 pass (one skip: `fixture_preparation_native`, needs KiCad's Python in Docker); the new tests: 10 audit cases and the index test (`tests/test_return_path.cpp`), `return_paths_cli`, and `viewer_stall`, which now also runs `returnpath_draw.test.ts` |
| `return-path-tests.txt`, `return-path-test-list.txt` | the `[return_path]` tests alone and their names |
| `viewer-node-tests.txt` | the Node tests of the viewer (24, no browser) |
| `asan.txt` | the same tests under ASan + UBSan, and the audit on two demo boards: no reports |
| `quick-compare.txt`, `planes-compare.txt`, `planes-soft-compare.txt` | route options off: routed boards byte-identical to `origin/main` (30/30, 8/8, 8/8). The router is not touched by this branch beyond the option and the post-route audit, so `mid` was not run again for it (it was for #8, whose router changes this branch sits on). CPU seconds are higher than the baseline's because other jobs shared the machine while these ran; the work and the output are the same |
| `index-equivalence.txt` | the grid index of the reference copper against the linear scans it replaced: unit tests and two real boards, byte-identical reports |

## Samples (`samples/`)

`royalblue-swd.{txt,json}`: the audit of the SWD, SPI, USB and clock nets of the RoyalBlue54L-Feather demo (7 nets). `timings.txt`: four demo boards, from 0.4 s to 38 s
(the 12-layer vme-wren demo, 236 nets, includes the engine's refill of 123 zones). Before the index of the reference copper the Jetson AGX Thor demo (291 nets) took 105 s and
vme-wren did not finish in 8 minutes.

## Looked at in a browser (`e2e/`)

Headless Chromium (Playwright's headless shell, SwiftShader WebGL2) against a live `tracemaker return-paths ... --view`: `e2e-return-paths.mjs`. The Return paths list showed
7 nets worst first (+4.1, +2.3, +2.1, +1.4, +1.0, +1.0, ok), a click on a net selected it and zoomed onto it (callouts: signal pad, assumed return pad, signal via, return via,
separation), `P` hid and showed the overlay, no console errors. `return-paths-all.png`, `return-paths-selected.png` and `return-paths-overlay-off.png` are screenshots of that run.

## Not verified

- Nobody has used the audit on a board of their own. The numbers are a geometric estimate (the shortest path of the return on the remaining reference copper), not a field
  solve; the acceptance picture of the issue (4.87 mm at the processor, 1.00 mm at the memory) was not reproduced because that board is not here.
- Arc tracks are skipped (a warning says so).
- Where a trace ends over a void the detour depends on which of several equally near boundary points is taken (up to half a millimetre on one SWD net): see the limits in doc 05 section 42.
- The look of the overlay (colours, callout placement) was judged on those screenshots only.
