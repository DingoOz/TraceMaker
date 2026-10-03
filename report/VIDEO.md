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
