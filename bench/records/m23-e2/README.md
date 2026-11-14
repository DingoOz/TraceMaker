# M23 E2: the plugin as a KiCad tool (D185)

2026-11-13, branch `features-2026-11-13`. KiCad 10.0.6 (Docker `kicad/kicad:10.0.6`, pcbnew under Xvfb), the plugin through the IPC API,
`scripts/kicad_gui_roundtrip.py` on `multichannel_mixer-unrouted.kicad_pcb`.

## Default settings (no `tracemaker.json`, `--time 60 --threads 8 --no-kb`)

```
TraceMaker: settings from defaults and TRACEMAKER_ARGS, TRACEMAKER_REPORT
power widths: 2 supply nets at 0.50 mm; 2 nets routed wide first: +12V 0.50, GND 0.50
power widths: 25 tracks wider than their class, 0 necks at pads
routed 118/141 connections, 531 tracks, 17 vias, pitch 0.100 mm, 15586696 expansions, 325.14 s (best variant 65.94 s)
TraceMaker: added 531 tracks and 17 vias, removed 27 old ones
TraceMaker: run report .../multichannel_mixer-unrouted.tracemaker-report.html
ok   the run report was written next to the board (multichannel_mixer-unrouted.tracemaker-report.html)
ok   the plugin ran against the live board
ok   copper changed in the open board: tracks 140 -> 645; vias 19 -> 35
ok   one undo remove the whole result: 140 tracks, 19 vias
ok   redo brings it back
ok   KiCad DRC of the board saved from the GUI: unconnected 148 -> 27
ok   no new DRC errors (none)
```

The report is `multichannel_mixer-unrouted.tracemaker-report.html`; the screenshot after routing is `routed.png`. Note the 325 s
against `--time 60`: the board is left open, so the relax rescue's two passes each get `--time` again (D182). A plugin user who sets 120 s
can wait about six minutes (doc 18 §20, E6).

## Route selected nets (settings read; `net_work` default)

```
TraceMaker: Net-(R14-Pad1): added 3 tracks and 0 vias, 2/2 routed, 0 still open (1.3 s, new session)
TraceMaker: vbias: added 48 tracks and 6 vias, 9/11 routed, 2 still open (3.7 s, warm session)
ok   later actions reuse the daemon's session (warm)
ok   one undo removes only the last added net's copper: (143, 19) (expected (143, 19))
ok   KiCad DRC of the board saved from the GUI: unconnected 148 -> 134
ok   no new DRC errors (none)
latency (s): 1.71, 4.01
```

## Can the zone refill be in the same undo step? (`refill-undo.log`)

`--refill after`: the plugin's refill after the commit (`TRACEMAKER_REFILL=1`). `--refill in-commit`: an experiment in which the plugin called
`board.refill_zones()` while its commit was still open.

| mode | undo 1 | undo 2 |
|---|---|---|
| after the commit | fills back, copper still there | copper gone: the input |
| inside the open commit | copper gone, fills still changed | fills back: the input |

Both take two undos: KiCad 10.0.6 records a refill as a step of its own. Inside the commit it is worse, since it fills before the new
copper is pushed. The plugin keeps the refill after the commit, and the experiment code was removed. The harness keeps `--refill after` as a check.

Offline: `kicad_plugin/test_offline.py` (settings defaults, file, environment, strict errors, binary chosen for options the module does
not take). ctest `pcm_package` and `pcm_package_bundled` pass with `tracemaker_settings.py` in the package.
