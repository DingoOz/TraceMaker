# Comparison video

`report/compare_AmpOne.mp4` shows TraceMaker, Freerouting "2.5.0-RC12" and Freerouting 1.9.0 routing the same
PCBench board (AmpOne, tier C) on the same machine, played at 4× real time, with each result's KiCad-DRC metrics at
the end.

How it is made:

1. TraceMaker records its routing events (every variant buffers; the winning variant's events are written):
   `build/release/src/app/tracemaker route <unrouted.kicad_pcb> -o build/video/tm.kicad_pcb --time 120 --threads 8 --no-kb --record build/video/tm_events.jsonl`
2. The events are rendered as frames (one per 4/15 s of routing):
   `build/report-venv/bin/python report/render_events.py build/video/tm_events.jsonl <unrouted.kicad_pcb> build/video/tm_frames --step 0.26667`
3. Freerouting's own GUI is screen-recorded on a private Xvfb display while it routes (published settings):
   `build/report-venv/bin/python bench/record_fr.py AmpOne_dev-AmpOne 2.5.0-RC12 build/video/fr25.mp4 --display 97`
   and the same with `1.9.0` → `build/video/fr19.mp4`.
4. Composition, metrics (each Freerouting session imported into the KiCad board and judged by kicad-cli):
   `build/report-venv/bin/python report/make_video.py --board AmpOne_dev-AmpOne --speed 4 --out report/compare_AmpOne.mp4`

Needs xvfb, ffmpeg, the JDK 25 in build/tools and python-xlib/matplotlib in build/report-venv. The jar named
2.5.0-RC12 in Freerouting's benchmark set reports itself as v2.4.2-SNAPSHOT (built 2026-09-29).

## Placement video

`report/compare_placement_SALSAFLOCK.mp4` (59 s) shows Freerouting "2.5.0-RC12" routing the designer's placement of
PCBench `retroreflectors_SALSAFLOCK` (tier B, 40 connections) next to TraceMaker placing the parts and then routing
them: (1) placement — the recorded stages of TraceMaker's winning candidate (`tracemaker-place --record`; positions
interpolated between recorded states), (2) routing at 1× real time — Freerouting's own GUI vs TraceMaker rendered
from its event log, (3) results judged by KiCad's DRC, with two controls (each router on the other placement).
Under both panels a strip shows the CPU threads in use, sampled from `/proc` while each job ran (`bench/cpu_sample.py`);
for Freerouting only the JVM's threads count.

How it is made:

1. `scripts/record_placement_video.sh` runs, one after another: `tracemaker-place --mode routable --record` under
   `bench/cpu_sample.py`; `tracemaker route --record` of the placed board (120 s limit, 8 threads) under the sampler;
   `bench/record_fr.py` (Freerouting GUI on Xvfb) under the sampler; `bench/dsn_place.py` (moves the `place` records
   of the board's `unrouted.dsn` to TraceMaker's placement, since kicad-cli cannot export Specctra) and Freerouting on
   that; and TraceMaker's router on the designer's placement.
2. `build/report-venv/bin/python report/make_place_video.py --board retroreflectors_SALSAFLOCK` renders the panels
   (`report/render_place.py`, `report/render_events.py`, the final boards with KiCad's violations drawn on them),
   composes 1920×1080 frames with PIL and encodes them; it writes `build/video_place/video_summary.json`.

Result on this board: Freerouting 35/40 routed with 56 new KiCad DRC errors (52 of them track width: KiCad's Specctra
export does not carry the minimum track width) vs TraceMaker 40/40 and 0; track length 720 vs 713 mm, vias 4 vs 14.
Placement's own share: TraceMaker's router routes 38/40 on the designer's placement and 40/40 on its own; Freerouting
stays at 35/40 on either. TraceMaker's routing job keeps 8 cores busy until its winning variant finishes (12 s), then
the slower variants run to the portfolio deadline (job 22 s); Freerouting routes on one thread, with its JIT compiler
threads busy alongside.
