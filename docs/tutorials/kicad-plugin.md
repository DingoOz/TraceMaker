---
status: current
code: [kicad_plugin, scripts/make_pcm_package.py]
---

# Route from inside KiCad

In this tutorial you install the TraceMaker plugin in KiCad 10, route the open board with one click, and undo it.
You need KiCad 10 and either a TraceMaker release or a build ([Route your first board](first-board.md), step 1).

!!! note "What has been checked"
    The plugin's round trip (tracks appear, one Ctrl+Z removes them, Ctrl+Y brings them back) is checked by
    `scripts/kicad_gui_roundtrip.py` in KiCad 10.0.6 running under Xvfb. Installing the package by hand in a desktop
    KiCad has not been tried by the project yet; tell us how it goes.

## 1. Get the package

**From a release (Linux x86_64).** Download `tracemaker-VERSION-pcm-linux-x86_64.zip` from the release page. It
carries its own copy of the router, so nothing else is needed.

**From your build.** Make the package and check it:

```bash
python3 scripts/make_pcm_package.py
python3 scripts/validate_pcm.py build/pcm/tracemaker-*.zip
```

This package does not contain the router. Tell the plugin where it is before you start KiCad:

```bash
export TRACEMAKER=$PWD/build/cpu-only/src/app/tracemaker
```

## 2. Install it

1. In KiCad 10, open **Preferences > Plugins** and tick **Enable KiCad API**. Plugins of this kind
   only run with it.
2. Open the **Plugin and Content Manager**, choose **Install from File...**, pick the zip and press **Apply**.
   KiCad creates a Python environment for the plugin (it installs `kicad-python`), which can take a minute.
3. Restart the PCB editor. **Autoroute with TraceMaker** appears in the toolbar and under
   **Tools > External Plugins**.

## 3. Route

Open a board with unrouted connections: `tests/boards/plane_smd/plane_smd.kicad_pcb` from the repository will do.

Press **Autoroute with TraceMaker**. The plugin saves a copy of the board with its project (so every design rule is
used), routes it with a 120 s limit, and adds the new tracks and vias to the open board as **one commit**. Your
board file is not overwritten until you save.

Press **B** to refill the zones, then run KiCad's **Inspect > Design Rules Checker**.

## 4. Undo and redo

One **Ctrl+Z** removes the whole result; **Ctrl+Y** brings it back.

## 5. Change the settings

The plugin reads its settings from environment variables, set where KiCad is started (a shell or a desktop file):

| To… | Set |
|---|---|
| Route for 60 s with 8 threads and watch it in the browser | `TRACEMAKER_ARGS="--time 60 --threads 8 --view"` |
| Remove the unlocked copper and route the whole board again | `TRACEMAKER_REROUTE=1` |
| Let TraceMaker move footprints first | `TRACEMAKER_PLACE=auto` |
| Refill the zones afterwards | `TRACEMAKER_REFILL=1` (the refill is its own undo step) |

The full list is in [`kicad_plugin/README.md`](../../kicad_plugin/README.md).

## What next

- Options worth trying for `TRACEMAKER_ARGS`: [the command line reference](../reference/cli.md), `route`.
- How the plugin talks to KiCad: [KiCad integration](../08-kicad-integration.md).
