# M18: the session daemon and "route selected nets" (D180, doc 19 §10)

2026-11-13, branch `roadmap-2026-11-13` on main `816634e`. Machine: 56 cores, RTX 5060 Ti; the router ran on the CPU reference
(the session's default).

## In a running KiCad 10.0.6

`build/plugin-venv/bin/python scripts/kicad_gui_roundtrip.py --nets N1,N2,...`: pcbnew from the Docker image `kicad/kicad:10.0.6`
under Xvfb, `kicad_plugin/tracemaker_route_nets.py` run against the live board through the IPC API, one pad of each net selected
before its action. Board `bench/data/kicad/demos/multichannel/multichannel_mixer-unrouted.kicad_pcb`.

| Run | Action | Session | Added | Wall time |
|---|---|---|---|--:|
| `roundtrip-r14-first.txt` | `Net-(R14-Pad1)` | new | 3 tracks, 2/2 routed | 1.75 s |
| | `vbias` | warm | 48 tracks, 6 vias, 9/11 routed | 3.78 s |
| `roundtrip-vbias-first.txt` | `vbias` | new | 48 tracks, 6 vias, 9/11 routed | 4.04 s |
| | `Net-(R14-Pad1)` | warm | 3 tracks, 2/2 routed | 1.59 s |
| | `vbias` again | warm | nothing (2 still open) | 0.88 s |

Both runs: one Ctrl+Z removes only the last net's copper, Ctrl+Y brings it back; KiCad's DRC of the board saved from the GUI:
unconnected 148 → 134, no new error. A warm session saves 0.2-0.4 s on these nets (the board read, the rules and the state's
caches); the rest is the router set-up each `route` call pays (about 0.8 s on this board) and the search. `nets-routed.png`: the
board after the first run.

## Without KiCad

`tests/integration/daemon_cli.py` (ctest `daemon_cli`): socket 0600 in a 0700 directory, a second daemon refused, a call answers
as `tracemaker session`, `diff` after a route and after an undo, reuse by fingerprint, bad requests answered, shutdown removes the
socket. In a session (`tracemaker session --jsonl`) on the same board: board read 1.0 s; `route` of the 2-connection net 0.86 s with
`verify: off` (57 expansions: the time is the router set-up), 1.68 s with the default `verify: refuse` (the DRC of the new state).
