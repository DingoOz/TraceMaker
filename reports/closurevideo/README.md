# TraceMaker placement closure video (60 s, 1080p30)

`tracemaker_closure_60s.mp4` (`_720p` for sharing) shows `tracemaker-place --mode closure` (D190, doc 04 §11) on mechkeys 58r
(PCBench), the board in `bench/records/m24-closure` that only closure brought to 100 %. Data from runs on 2026-11-14 with the
`TraceMaker-rm3` build (1df17ba, router unchanged since 975cb00).

| time | shot | source |
|---|---|---|
| 0-5 s | title | |
| 4.6-17.6 s | the designer's placement (`human.kicad_pcb` = PCBench `unrouted.kicad_pcb`) routed at 10 M work, 8 threads, `--no-kb`: replay of `human.jsonl`, cross-faded to the board as written; the 6 open connections from `human.json` marked | 355/361 (`human.json`); 357/361 at 40 M from `bench/records/m24-closure/closure-human40m` |
| 17.2-39.6 s | the closure loop's 11 trials as logged in `closure.placed.json` (trials, arms, before/after, kept). Pads only, zoomed to 27-113 x 44-96 mm. Kept changes move parts from the designer's pose to the closure pose; R3's turn is shown at trial 2, PROGRAM's shift at trial 11, the other 14 moved parts at the kept window trial 9 | `closure.placed.json`, footprints of `human.jsonl` / `closure.jsonl` board events |
| 39.2-48.6 s | the closure placement (`closure.kicad_pcb` = closure-route10m's placed board) routed with the same settings: replay of `closure.jsonl` | 361/361 (`closure.json`) |
| 48.2-57.6 s | KiCad 10.0.6 DRC (`scripts/kicad-cli`, errors only) on both routed boards and their unrouted inputs | `human.drc.json`: 6 unconnected, 184 errors = 184 on the input; `closure.drc.json`: 0 unconnected, 183 = 183 on its input (one courtyard overlap fewer than the designer's: 2 -> 1). 14-board line from `bench/records/m24-closure/table.md` |
| 57.2-60 s | end card | |

Rebuild: route both boards with `tracemaker route X.kicad_pcb -o X.rec.kicad_pcb --work 10000000 --no-kb --time 3600 --threads 8
--json X.rec.json --record X.jsonl` (X = human, closure; without `--view`: `--record` is ignored when `--view` is on,
`src/app/route_job.cpp:403`), then `python3 video.py frames 24` and
`ffmpeg -framerate 30 -i frames/f_%04d.png -c:v libx264 -preset slow -crf 18 -pix_fmt yuv420p -movflags +faststart tracemaker_closure_60s.mp4`.
`python3 video.py still SECONDS...` renders single frames.

Honest notes:
- The routes in the video were re-run for the recording; `human.routed.kicad_pcb` / `closure.routed.kicad_pcb` (same settings, with
  `--view`) are byte-identical apart from uuids, and match the benchmark's counts.
- The board's 183-184 DRC errors are the input file's own (drill sizes, edge clearance, mask bridges); routing adds none.
- The loop took 24 minutes (1,445 s) on this board; the trial list is paced for reading, not to scale.
- Which of the 14 window-moved parts moved in the rejected window trial 7 is not logged, so that trial outlines nothing.
