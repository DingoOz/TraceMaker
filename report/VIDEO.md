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

`report/compare_placement_SALSAFLOCK.mp4` (62 s) compares Freerouting "2.5.0-RC12" routing the designer's placement of
PCBench `retroreflectors_SALSAFLOCK` (tier B, 40 connections) with TraceMaker placing the parts and then routing them.

1. **Finished boards** — both results in 3D (`kicad-cli pcb render`, `scripts/render_board_spin.py`), one full turn.
   These footprints carry no 3D part models, so the renders show board, copper, pads and silkscreen.
2. **Placement** — Freerouting does not place parts, so its input is the designer's hand placement (time not recorded;
   the video does not invent one). TraceMaker places on its own clock (142 s, played at 10×): the panel shows the
   candidate that was finally kept as it was at each moment (annealing snapshots carry their own times;
   `tracemaker-place --record`), captioned with what the job was doing (building candidates, router checks,
   re-placement, ECO trials, verification routes).
3. **Routing** — one clock for both, starting when routing starts, at real time. Freerouting's span is taken from its
   log ("Starting routing" to "Saving", 14.5 s; JVM start-up and loading excluded) and its screen recording is cut to
   match; TraceMaker is rendered from `tracemaker route --record` (best variant done at 12 s, job ends at 24 s while
   the slower variants run to their deadline). When a tool finishes, its panel shows the saved KiCad board.
   Vertical meters show each busy CPU thread and, for TraceMaker, the SM use of both GPUs, sampled while the jobs ran
   (`bench/cpu_sample.py --gpu`); Freerouting's JVM JIT-compiler and GC threads are drawn dimmer than its one routing
   thread. The elapsed time is the largest number.
4. **Result** — both boards with KiCad's violations drawn on them and the score card, with two controls (each router on
   the other placement).

How it is made: `scripts/record_placement_video.sh [board]` (placement, routing and Freerouting under the sampler,
one after another, plus the controls; `bench/dsn_place.py` writes the Specctra file for Freerouting on TraceMaker's
placement since kicad-cli cannot export Specctra), `python3 scripts/render_board_spin.py` (after the composer has
imported Freerouting's session once, or run `bench/ses_import.py` first), then
`build/report-venv/bin/python report/make_place_video.py --board retroreflectors_SALSAFLOCK`.

Result on this board (KiCad DRC): Freerouting 35/40 routed with 56 new errors (52 track width: KiCad's Specctra export
does not carry the minimum track width) in 14.5 s of routing; TraceMaker 40/40 with 0 new errors, placed in 142 s and
routed in 12 s (job 24 s). Track length 720 vs 713 mm, vias 4 vs 14. Placement's own share: TraceMaker's router routes
38/40 on the designer's placement and 40/40 on its own; Freerouting stays at 35/40 on either. TraceMaker used CUDA:
the router's cost-to-go fields run on both GPUs (P100 and V100) in every variant, including the placement's check
routes; the placer's annealing and legalisation are CPU-only. Freerouting is CPU-only.

### Second board: `report/compare_placement_aquarius.mp4`

PCBench `kitspace_aquarius` (tier A, 171 connections; TM4C123 MCU, micro-USB, LDO, 16 MHz crystal, 13 LED driver
stages), 80 s. Made with `scripts/record_placement_video.sh kitspace_aquarius`,
`python3 scripts/render_board_spin.py build/video_place/kitspace_aquarius` and
`make_place_video.py --board kitspace_aquarius --caveat "..."`. Routing is played at 2× for both tools (TraceMaker's
job takes 82 s).

TraceMaker kept its full placement (72 parts moved, ratsnest 2,487 → 1,085 mm) in 181 s and routed 171/171 with 0 new
KiCad DRC errors, 1,017 mm and 62 vias (best variant at 64 s, job 82 s). Freerouting on the designer's placement:
171/171, 9 new errors (6 track width, 3 copper-edge clearance), 2,472 mm, 75 vias, 42 s. Controls: TraceMaker's router
on the designer's placement 2,217 mm / 82 vias; Freerouting on TraceMaker's placement 1,225 mm / 54 vias — the
placement halves the track length for either router.

Freerouting 2.5's autorouter pass is single-threaded, but its optimizer is not ("optimizer.max_threads=55" in its log
even with `--router.max_threads=1`): its batch-optimizer threads used 186 CPU-seconds in 13 s here. The video shows
Freerouting's two phases from its log. (On SALSAFLOCK the optimizer was skipped because connections stayed unrouted.)

Caveat shown on the card: the placer optimises wiring only. It moved the indicator LEDs D1–D13 away from their
silkscreen labels ("choose_indication", "control_indication"), which a designer would not accept; design-intent rules
of that kind are planned in doc 15 (M13).
