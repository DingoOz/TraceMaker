# Issue #5 records: footprint check against datasheet land patterns

Design: [docs/05-routing.md](../../../docs/05-routing.md) section 43, protocol [docs/13-viewer-protocol.md](../../../docs/13-viewer-protocol.md) (`footprint_check`), overlay
[docs/09-visualization.md](../../../docs/09-visualization.md), decision D115. Base: the issue-7 branch (PR #35), which this builds on (the `tm::analysis` library, the viewer's
side-panel and overlay helpers). Examples of the reference format and of a trusted brand list: [docs/footprint_refs/](../../../docs/footprint_refs/).

## What it does, in one paragraph

`tracemaker footprint-check BOARD --refs DIR` (and `route --footprint-refs DIR`, before the first track) compares each footprint with a **reference**: a record of the datasheet's
land pattern with its brand, document, revision and page, as JSON or the manufacturer's `.kicad_mod`. Only references whose **brand is on a trusted list** are used; any other
reference gives "untrusted", never a verdict. The pattern is aligned to the footprint (translation, quarter turns, mirror only as the footprint's side requires) and every pad's number,
position, size, drill and type is compared: any difference is an error. It does not read datasheets and does not touch the network.

## What was checked (`verification/`)

| File | What |
|---|---|
| `ctest-release.txt` | the full suite, release preset: all pass (one skip: `fixture_preparation_native`); the new tests: 22 cases in `tests/test_footprint_check.cpp`, `footprint_check_cli`, and `viewer_stall`, which now also runs `footprintcheck_draw.test.ts` |
| `footprint-check-tests.txt` | the `[footprint_check]` tests alone |
| `viewer-node-tests.txt` | the Node tests of the viewer (29, no browser) |
| `asan.txt` | the tests, the CLI test (it routes) and two demo boards under ASan + UBSan: no reports |
| `quick-compare.txt`, `planes-compare.txt`, `planes-soft-compare.txt` | route options off: routed boards byte-identical to `origin/main` (30/30, 8/8, 8/8). `mid` was not run again: this branch does not touch the router, only a guarded block before it. CPU seconds are higher than the baseline's because other jobs shared the machine while these ran; the work and the output are the same |

## Real boards (`real-boards/`)

`results.txt`, `selfcheck-results.txt` and the scripts that made them (`sx.py` reads the boards, `flipcheck.py`, `selfcheck.py`, `negative_control.py`). **These test the alignment,
rotation and flip logic, not any datasheet: the references are derived from the boards themselves.**

- `selfcheck.py` on nine KiCad demo boards: a reference from the first front instance of every library footprint, compared with every instance: 3,144 footprints (front and back, every
  rotation they had) `ok`, none called out.
- `flipcheck.py`: the library footprints that are on both sides of video, CM5_MINIMA_3, vme-wren and the Jetson baseboard: 2,257 footprints `ok`, the back ones aligned mirrored.
- `negative_control.py`: the same references with a pad moved by 0.2 mm and another widened by 0.3 mm: called out on all 32 footprints they applied to (CM5_MINIMA_3).
- `footprint-check` takes about a second on vme-wren (1,500 footprints).

The first versions of these scripts produced "mirrored" and drill errors; they were the scripts' own mistakes (references from back-side instances, oval drills read as round) and the
check was right about what it was given. They are fixed in the scripts that are here.

## Looked at in a browser (`e2e/`)

Headless Chromium (Playwright's headless shell, SwiftShader WebGL2) against a live `footprint-check ... --view` on CM5_MINIMA_3 with the corrupted references: `e2e-footprint-check.mjs`.
The Footprint check list showed 32 footprints with two errors each, a click on D401 selected it (and scrolled it into view), zoomed onto it, drew a cross on pad 1 and pad 3 with the labels
`pad 1: 0.2 mm off` and `pad 3: 0.25x0.68 vs 0.55x0.68 mm`, and listed the sentences and the datasheet; `D` toggled the overlay; no console errors. `footprint-check-all.png`,
`footprint-check-selected.png`, `footprint-check-overlay-off.png`.

## Not verified

- No list of references ships with the tool and nobody has run it against references made from real datasheets on a board of their own. The example in `docs/footprint_refs/` is
  synthetic (a made-up brand and part).
- The check is as good as the reference: a typo in a record is a typo in the verdict. A trusted brand is trusted because the list says so.
- Only copper pads and non-plated holes are compared (not courtyard, silkscreen, paste, mask, 3D model or the symbol's pin functions); pads at arbitrary angles are not compared by size.
- Footprints are found by part number (a property called MPN or the like), library id, value or designator; the datasheet link in a footprint's properties is not read.
- The wording of the messages and the look of the overlay were judged on these screenshots only.
