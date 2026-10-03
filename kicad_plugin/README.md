# TraceMaker KiCad plugin (IPC API, KiCad 10)

The action **Autoroute with TraceMaker** saves a copy of the open board (with its project, so all design rules are
available), routes every unrouted connection, and adds the new tracks and vias to the open board as **one commit**:
a single Ctrl-Z removes the whole result. The open board file is never overwritten.

## Install

KiCad 10 runs IPC plugins only with the API server enabled: Preferences > Plugins > "Enable KiCad API".
KiCad creates a virtual environment for the plugin from `requirements.txt` (kicad-python 0.8).

**From the PCM package (recommended).** Build the package and install it with KiCad's Plugin and Content Manager:

```
python3 scripts/make_pcm_package.py          # -> build/pcm/tracemaker-<version>.zip
python3 scripts/validate_pcm.py build/pcm/tracemaker-<version>.zip
```

In KiCad: Plugin and Content Manager > **Install from File...** > choose the zip > Apply. The action appears in the
PCB editor's toolbar and under Tools > External Plugins.

**By hand.** Copy this folder to `${KICAD_DOCUMENTS_HOME}/10.0/plugins/tracemaker/`.

## The engine

The plugin does not contain the router by default; it finds it in this order:

1. The Python module `tracemaker` (built as `build/release/python/tracemaker.cpython-*.so` when pybind11 is
   installed). It is used in-process when importable: set `TRACEMAKER_PYTHONPATH` to its directory, or put it in
   `lib/` next to `tracemaker_route.py`. The module must match the Python version of KiCad's plugin environment
   (it is built for the system `python3`); if it cannot be imported the plugin silently uses the binary.
2. The `tracemaker` binary: `TRACEMAKER` (full path), else `bin/tracemaker` next to `tracemaker_route.py`, else
   `tracemaker` on `PATH`.

Both paths run the same code (`tmk::app::run_route_job`) and produce identical results.

Environment variables (set them where KiCad is started, e.g. in the shell or a desktop file):

| Variable | Meaning |
|---|---|
| `TRACEMAKER` | path to the `tracemaker` binary |
| `TRACEMAKER_PYTHONPATH` | directory containing the `tracemaker` Python module |
| `TRACEMAKER_ARGS` | route options in CLI spelling, default `--time 120`; e.g. `--time 60 --threads 8 --view` (`--view` streams the routing to the browser viewer) |
| `TRACEMAKER_NO_BINDINGS=1` | always use the binary |

**Bundling (Linux, same machine or distribution only).** The package can carry the engine so that no environment
variable is needed:

```
python3 scripts/make_pcm_package.py --strip --binary build/release/src/app/tracemaker \
    --module build/release/python/tracemaker.cpython-314-x86_64-linux-gnu.so
```

This marks the package `platforms: ["linux"]`. The binaries link against the system's CUDA runtime, SQLite and
libstdc++, so such a package only works where those match. Binaries are never committed to the repository.

## Python module

```python
import tracemaker
b = tracemaker.read_board("board.kicad_pcb")      # nets, footprints, pads, tracks, vias, copper_layers, outline_mm
r = tracemaker.route("board.kicad_pcb", "routed.kicad_pcb", time_s=60, threads=8)   # same as `tracemaker route`
r["routed"], r["connections"], r["unrouted"]
tracemaker.drc("routed.kicad_pcb")                 # list of violations (KiCad type names)
tracemaker.emit_items("board.kicad_pcb", work=2_000_000)   # new copper, `route --emit-items` layout
```

`route()` takes `work=` (deterministic budget), `seed=`, `kb=` (knowledge base: True, False or a path), `gpu=`,
`view=`, `verbose=` and the other CLI options; the GIL is released while routing.

## Tests

- `test_offline.py ITEMS_JSON`: TraceMaker's `--emit-items` output becomes valid kipy Track/Via objects, and
  `TRACEMAKER_ARGS` maps onto the module's keyword arguments (kicad-python 0.8, no KiCad needed).
- `bindings/test_bindings.py` (ctest `python_bindings`): module against CLI, byte-identical boards.
- ctest `pcm_package`: builds and validates the PCM archive.

Status: the full round trip needs a running KiCad GUI (KiCad 10 has no headless IPC server) and has not been
exercised yet; installing the PCM archive in a KiCad GUI is likewise untested. The archive passes kicad-python
0.8's own PCM validator (`python -m kipy.packaging validate`), which `validate_pcm.py` runs when kipy is installed.
