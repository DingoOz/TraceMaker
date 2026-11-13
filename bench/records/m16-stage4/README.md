# M16 stage 4: placement advice (D178, doc 17 §8.3)

2026-11-13, branch `roadmap-2026-11-13` on main `816634e`.

## Oracle

`scramble.py BOARD OUT ADVICE 3` turns the three movable parts with the most pads (connectors, switches, holes and test points
excluded) by 180 degrees in a copy of a PCBench board and writes advice that puts them back: one seed with the three moves, and the
three as ECO moves. `run.sh` runs `tracemaker-place --mode routable --route-check 1000000 --route-threads 4 --rounds 1
--eco-candidates 2 --final-work 0` with (`*.adv.*`) and without (`*.ctl.*`) `--place-advice`; `*.eadv.*` / `*.ectl.*` are
`--mode eco --eco-rounds 2 --eco-candidates 2`.

| Board | Turned input | Without advice | With advice | What the advice did |
|---|--:|--:|--:|---|
| `fan_controller` (routable) | 4 unrouted | 0 | 0 | seed skipped: 1 overlap in the placer's conservative model |
| `fan_controller` (eco) | 4 | 0 (U1 rotate 180), 3 routes | 0 (advised U1 move), 5 routes | advised move routed first and committed; ECO's ranking finds the same |
| `spisolator` (routable) | 0 | 0, HPWL 162.4 mm | 0, HPWL 162.4 mm | seed routed (0 unrouted, 182.0 mm), not kept |
| `esp8266_envmonitor` | 8 | 8 | refused | `U1 may not move (overhangs the board edge in the input)` |

## Smoke test

`smoke-1bitsy-*`: `1Bitsy_1bitsy`, advice moving C1 by 0.5 mm (seed) and an ECO move; the seed adds an overlap and is skipped with
the reason in the report's `advice` block.
