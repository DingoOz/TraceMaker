---
status: current
code: [src/core/live.hpp, src/core/live.cpp, bench/live_run.py, devsite/livefeed.py, devsite/static/live.html]
---

# Watch runs live

Open `http://<host>:8765/live`. The page lists everything TraceMaker is doing on the machine and updates once a second:

- **Bench runs**: the set, branch, worktree, commit, budget and jobs. A progress bar shows the boards finished, running and
  queued. A card for each board in flight shows its step: placing, routing or judging with KiCad. A table lists the finished
  boards with their routed count and DRC result.
- **Each routing process**:
  - the board, stage (routing, relax, rescue, after-route passes, writing) and round;
  - the best variant's routed count, with a chart of it over time;
  - a row per portfolio variant: phase, routed, work against the round's budget, work over all rounds;
  - CPU use, memory, threads and a link to its viewer when it runs with `--view`;
  - the log's last lines and the full command line.
- **The machine**: load, memory and the GPUs.

Ended runs stay on the page for 30 minutes; the selector at the top keeps them for 10 minutes up to 24 hours.

Nothing needs turning on. Any `tracemaker` built from a checkout with D165, and any `bench/run.py` from one, publishes its state
to `~/.cache/tracemaker/live`. The progress site reads it whichever checkout or worktree the run came from. To keep one run
off the page, give it `TM_LIVE=0`. To keep a run's files somewhere else, set `TM_LIVE_DIR` for the run and for the progress site.

To serve the page from a checkout other than the one the `tracemaker-devsite` service runs from:

```
python3 devsite/server.py --port 8772    # then open http://<host>:8772/live
```

The page reads `GET /api/live?keep=SECONDS`: the processes and runs as JSON, with the bench runs' own processes under `procs`.
