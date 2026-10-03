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

`report/compare_placement_SALSAFLOCK.mp4` (56 s) compares Freerouting "2.5.0-RC12" routing the designer's placement of
PCBench `retroreflectors_SALSAFLOCK` (tier B, 40 connections) with TraceMaker placing the parts and then routing them.

1. **Finished boards** — both results in 3D (`kicad-cli pcb render`, `scripts/render_board_spin.py`), one full turn.
   These footprints carry no 3D part models, so the renders show board, copper, pads and silkscreen.
2. **How they were made, on one clock** — played at a constant 5× for both. Freerouting routes from t = 0 (its own GUI,
   screen-recorded). TraceMaker places from t = 0 and routes after: the placement panel shows the candidate that was
   finally kept, as it was at each moment (annealing snapshots carry their own times; `tracemaker-place --record`),
   captioned with what the job was doing (building candidates, router checks, re-placement, ECO trials, verification
   routes); routing is rendered from `tracemaker route --record`. When a tool finishes, its panel shows the saved
   KiCad board. Vertical meters show each busy CPU thread and, for TraceMaker, the SM use of both GPUs, sampled while
   the jobs ran (`bench/cpu_sample.py --gpu`: `/proc` per thread every 0.25 s, `nvidia-smi pmon` per process every 1 s;
   for Freerouting only the JVM's threads count). The elapsed time is the largest number; a bar shows each tool's
   phases on the shared clock.
3. **Result** — both boards with KiCad's violations drawn on them and the score card, with two controls (each router on
   the other placement).

How it is made: `scripts/record_placement_video.sh [board]` (placement, routing and Freerouting under the sampler,
one after another, plus the controls; `bench/dsn_place.py` writes the Specctra file for Freerouting on TraceMaker's
placement since kicad-cli cannot export Specctra), `python3 scripts/render_board_spin.py` (after the composer has
imported Freerouting's session once, or run `bench/ses_import.py` first), then
`build/report-venv/bin/python report/make_place_video.py --board retroreflectors_SALSAFLOCK`.

Result on this board (KiCad DRC): Freerouting 35/40 routed with 56 new errors (52 track width: KiCad's Specctra export
does not carry the minimum track width) in 22 s; TraceMaker 40/40 with 0 new errors after 142 s placing and 24 s
routing (166 s in all). Track length 720 vs 713 mm, vias 4 vs 14. Placement's own share: TraceMaker's router routes
38/40 on the designer's placement and 40/40 on its own; Freerouting stays at 35/40 on either. TraceMaker used CUDA:
the router's cost-to-go fields run on both GPUs (P100 and V100) in every variant, including the placement's check
routes; the placer's annealing and legalisation are CPU-only. Freerouting is CPU-only.
