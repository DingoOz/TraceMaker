---
status: current
code: [src/app, tests/boards/plane_smd]
---

# Route your first board

In this tutorial you build TraceMaker, route a small four-layer board that comes with it, and check the result with a
design-rule check. It takes about fifteen minutes, most of it the build. You need Linux, CMake, Ninja and GCC 13 or
newer; no GPU and no KiCad.

!!! tip "No compiler?"
    A tagged release carries a ready-built Linux tarball (`tracemaker-VERSION-linux-x86_64.tar.gz`) with the same
    example board in `share/tracemaker/examples/plane_smd/`. Unpack it, put its `bin/` on your `PATH`, and start at
    step 2 with `tracemaker` in place of `build/cpu-only/src/app/tracemaker`.

## 1. Build

From the top of the repository:

```bash
cmake --preset cpu-only
cmake --build --preset cpu-only
build/cpu-only/src/app/tracemaker version
```

The last line prints `tracemaker 0.10.0` (or a newer version). The `cpu-only` preset builds without CUDA; with a
CUDA 12.x GPU you can use `release` instead, and the results are the same (see
[Use the GPUs](../how-to/gpu.md)).

To keep the commands short, name the program once:

```bash
TM=build/cpu-only/src/app/tracemaker
```

## 2. Look at the board

The example is `tests/boards/plane_smd/plane_smd.kicad_pcb`: a 24 × 14 mm board with every part on the top side, a
ground plane on `In1.Cu` and a 3.3 V plane on `In2.Cu`. It has a 0.4 mm-pitch ball array, a QFN microcontroller and a
32.768 kHz crystal, and no tracks yet.

```bash
$TM inspect tests/boards/plane_smd/plane_smd.kicad_pcb
```

```text
tests/boards/plane_smd/plane_smd.kicad_pcb
  format 20260206 (named nets), 4 copper layers: F.Cu In1.Cu In2.Cu B.Cu
  11 nets, 16 footprints, 91 pads, 0 tracks, 0 arcs, 0 vias, 4 zones
  outline 24.000 x 14.000 mm
```

Check it before routing, so you know what "better" will mean:

```bash
$TM drc tests/boards/plane_smd/plane_smd.kicad_pcb
```

It reports `unconnected_items 47`: nothing is routed yet. The design rules come from `plane_smd.kicad_pro` next to
the board, as they do in KiCad.

## 3. Route it

Make a folder for the result (`build/` is ignored by git) and copy the project file into it, so the routed board keeps its design rules:

```bash
mkdir -p build/tutorial
cp tests/boards/plane_smd/plane_smd.kicad_pro build/tutorial/routed.kicad_pro
$TM route tests/boards/plane_smd/plane_smd.kicad_pcb -o build/tutorial/routed.kicad_pcb \
    --soft-zones --via-in-pad --keep-vias-off-pads --first-nets XIN,XOUT \
    --work 3000000 --variants 4 --no-gpu
```

What the options are for, on this board:

| Option | Why |
|---|---|
| `--soft-zones` | No pad touches a plane, so every ground and 3.3 V pad needs a via down into its plane. Zone fills stop blocking other nets, and the job refills them afterwards to check that nothing was cut off. |
| `--via-in-pad` | One inner ball of the ball array has no other way out than a via in its pad. |
| `--keep-vias-off-pads` | No plane vias in the small 0402 and QFN pads. |
| `--first-nets XIN,XOUT` | The crystal lines go first, so they stay short. |
| `--work 3000000` | A budget in search steps instead of seconds, so you get the same result every time, at any thread count. |
| `--variants 4` | Run four of the eight differently configured routers and keep the best result. |
| `--no-gpu` | Compute on the CPU only. The results are the same either way. |

After about five seconds the last lines read:

```text
  variant 2 fast bends, longest first (2x pitch on large boards) routed 44 in 4.7 s
  variant 1 fast bends, shortest first, crowded source routed 47 in 3.3 s
soft zones: after refilling 2 zones, 0 items unconnected
routed 49/49 connections, 108 tracks, 42 vias, pitch 0.025 mm, ...
```

Every connection is routed. The exit code is 0 when nothing is left open.

## 4. Check the result

The written board keeps its old zone fills (TraceMaker does not change what it did not create). Refill them for
the check, the way KiCad does before its own check:

```bash
$TM drc build/tutorial/routed.kicad_pcb --refill-zones
```

It prints `zones refilled 2` and no violations. With `--json report.json` it writes the report in `kicad-cli`'s
layout.

TraceMaker's own check is fast but is not the judge: KiCad's is. If you have KiCad 9 or 10, or Docker
([Run kicad-cli from Docker](../how-to/kicad-cli-docker.md)):

```bash
kicad-cli pcb drc --refill-zones --format json -o build/tutorial/drc.json build/tutorial/routed.kicad_pcb
```

KiCad 10.0.6 prints `Found 33 violations` and `Found 0 unconnected items`. The 33 are silkscreen warnings
(`silk_over_copper`, `silk_overlap`, `silk_edge_clearance`) that the unrouted board already had; routing added no
error. Comparing with the check of the input board, rather than looking for zero, is how the benchmarks judge too.

Open `build/tutorial/routed.kicad_pcb` in KiCad to look at it, and press **B** to refill the zones there.

## 5. Watch it route (optional)

The live viewer is a web page. Build it once (needs Node.js), then route with `--view --hold`:

```bash
(cd viewer && npm install && npm run build)
$TM route tests/boards/plane_smd/plane_smd.kicad_pcb -o build/tutorial/routed.kicad_pcb \
    --soft-zones --via-in-pad --keep-vias-off-pads --first-nets XIN,XOUT --view --hold
```

Open `http://localhost:8766/`. `--hold` keeps the page up after the route ends; Ctrl-C stops it.

## What next

- Route your own board: `$TM route board.kicad_pcb -o out.kicad_pcb --time 120`. The README's *Common tasks* table
  lists the options worth trying first, and [the command line reference](../reference/cli.md) has all of them.
- [Route from inside KiCad](kicad-plugin.md) with the plugin.
- Read [how the router works](../05-routing.md).
