---
status: current
code: []
---

# TraceMaker

**An autorouter and component placer for KiCad.** You give it a `.kicad_pcb`; it draws the tracks and vias and
writes them back into the file. KiCad's own design-rule check is the judge, and anything TraceMaker did not create
round-trips byte for byte.

![TraceMaker's live viewer after routing a motor-driver board: 195 of 195 connections](img/viewer.png)

## Where to go

This documentation has four kinds of page, after the [Diátaxis](https://diataxis.fr/) framework. Pick by what you
want to do now.

| I want to… | Go to |
|---|---|
| Get it running in five minutes, on Linux, macOS or Windows | [Quick start](quick-start.md) |
| Try it for the first time, step by step | [Tutorials](tutorials/index.md): route your first board, route from inside KiCad |
| Get a particular job done | [How-to guides](how-to/index.md): build and test, run `kicad-cli` from Docker, use the GPUs, run benchmarks, release, write docs |
| Look up an option, a message or a number | [Reference](reference/index.md): every command-line option, the viewer protocol, benchmark results, the C++ API |
| Understand how and why it works | [Explanation](explanation/index.md): requirements, architecture, placement, routing, GPU, the papers behind them |
| Follow the project | [Project](project/index.md): roadmap, candidate features, the [decision records](12-decisions.md) |

## Status

Release 0.10.0. Linux (C++20); a CUDA 12.x GPU speeds it up and is never required. Licence GPL-3.0-or-later. How good
it is, measured, and its known limits are in the [README](../README.md#how-good-is-it) and the
[benchmark results](reference/benchmarks.md).

## Status labels on these pages

Each design document carries a status, shown at the top of the page:

- **current**: describes the code as it is.
- **partly built**: part of it is built; the page says which part. The rest is a plan.
- **plan**: nothing in it is built yet, or it is a proposal.
- **historical**: kept for the record; the code has moved on.
