---
name: tracemaker-route
description: Route a prepared KiCad 10 board with the TraceMaker autorouter and sign it off with KiCad's refilled DRC. Chooses options (work budget, soft zones, vias off pads, differential pairs), reads the route summary, compares KiCad DRC against the unrouted baseline and decides whether the result is clean, incomplete or illegal. Use after tracemaker-board-prep, when the user wants a board autorouted.
---

# Routing with TraceMaker

## For humans

TraceMaker routes the unrouted connections of a placed KiCad board and writes a new `.kicad_pcb`.
It never moves or rips locked items and keeps everything it did not touch byte for byte. KiCad stays the
judge: a route is finished only when KiCad's DRC, after refilling the zones, reports no unconnected items
and no errors the unrouted board did not have.

Prepare the board first (`tracemaker-board-prep`).

## For agents

### Inputs

- A prepared board (preflight run, blocks fixed) with its `.kicad_pro`/`.kicad_dru` beside it.
- The baseline DRC of that unrouted board (board-prep step 5).
- The `tracemaker` binary (`tracemaker version`; build instructions are in the TraceMaker README).

Never write over the canonical board. Route to a new file, check it, then promote it with a backup. KiCad
finds a board's project by its file name, so the routed file is judged on default rules where it lies;
the sign-off script judges a copy beside the input's project files instead.

### 1. Choose options

Start from the preflight's suggested command, then:

| Situation | Options |
|---|---|
| Any run you want to reproduce | `--work N` (deterministic: same input, same options, same output on any machine). `--time` is only a wall-clock safety net per variant, default 120 s |
| Board with ground/power planes on inner layers | `--soft-zones --keep-vias-off-pads` (cut and connect into planes; keep vias out of SMD pads narrower than 2 mm, `--vias-off-pads-below MM` for another limit) |
| Routing deleted from a board with pours | `--soft-zones`: the stale fills' voids fit only the old tracks |
| Two-layer board with pours | Try with and without `--soft-zones` and keep the better sign-off: at 3M work it helped StickHub (30 → 15 open) and multichannel_mixer (11 → 4), not pic_programmer (6 → 7) or sonde xilinx (0 → 2) |
| Inner layers that must stay whole planes | `--no-tracks-on In1.Cu,In2.Cu` (vias still pass and reach fills there), or `--layer-cost In1.Cu=4` to make them dearer |
| Differential pairs (`+`/`-`, `P`/`N` names) | `--diff-pairs`; `--pair-skew-mm X` for a skew limit |
| Crystal or other short critical nets | `--first-nets XIN,XOUT` (routed first, never ripped by other nets) |
| Dense BGA / QFN | Escape planning runs in two of the eight portfolio variants by default; `--escape-plan` turns it on in all. `--escape-report` lists which deep BGA pins connected. `--via-in-pad` lets inner balls take a minimum via in the pad (needs filled, capped vias at the fab) |
| Blind/buried or micro vias allowed by the board | `--blind-vias`, `--micro-vias` (used only where a through via is blocked) |
| Component-aware keep-outs (crystals, switcher inductors) | `--component-rules report` writes a sidecar `.kicad_dru` to review; `on` also routes with the keep-outs |

Work budget: start with `--work 10000000` for a board of a few hundred connections, measure, then raise.
More work is not always better on soft-zone boards (the router's count is not KiCad's after the refill), so
judge each run and keep the best, not the last. `--variants` (default: all with `--work`) runs a portfolio of router
configurations and keeps the best; `--threads` only changes speed when `--work` is set, never the result.

```
tracemaker route BOARD.kicad_pcb -o BOARD-routed.kicad_pcb --json route.json --work 10000000 [options]
```

Exit code 0 means every connection the router planned was routed; 3 means some were not.

### 2. Read the summary

`route.json`: `routed` / `connections`, `tracks`, `vias`, `expansions`, `failures` (one line per unrouted
connection, with the reason: boxed in, out of budget, ...) and the portfolio variant kept. The router's count
is not sign-off: with soft zones, plane connections exist only once KiCad refills.

### 3. Sign off with KiCad

```
python3 <this skill>/scripts/signoff.py BOARD.kicad_pcb BOARD-routed.kicad_pcb --route-json route.json
```

It runs `kicad-cli pcb drc --refill-zones` on both boards, with the input's `.kicad_pro` / `.kicad_dru`, and
reports unconnected items before and after, errors the routing added (by type, with examples), and a
verdict: `CLEAN` (exit 0), `INCOMPLETE` (1), `ILLEGAL` (2). Errors present in the unrouted board are not
counted against the route.

| Added error | Cause | What to do |
|---|---|---|
| `starved_thermal` | New copper blocked thermal spokes; the router does not model them | Re-route near that pad, widen spokes, or fix by hand |
| `solder_mask_bridge` near a logo | The router ignores unfilled mask circles | Rule area over the logo, re-route |
| `clearance` / `shorting_items` with a zone | Plane cut or refill effect | Check zone priority and clearance; re-route with different options |
| Anything else involving new tracks or vias | A router defect | Report it with the board and command |

KiCad's `solder_mask_bridge` count can differ between two runs on the same refilled board; rerun the
sign-off before blaming the router for a change of one or two.

Unconnected items left: raise `--work`, try the other zone mode, check the preflight's escape report (pins
walled in by custom rules stay unrouted until the rule allows them), or route the remainder by hand. Do not
loosen design rules to make DRC pass.

### 4. Hand back to KiCad

- If the user had the board open, tell them to close without saving before promoting the routed file over
  the canonical one, then reopen.
- The routed zone fills are stale where new copper cut them: after promoting, refill in KiCad (`B`) and
  save before reviewing or exporting. A refill needs the board's own project, so refill the promoted file,
  not the routed copy.
- Regenerate teardrops if the design uses them; add ground stitching vias after routing.
- Finish with the project's export flow (kistack `kicad-export`: refilled DRC with
  `--schematic-parity`, Gerbers, BOM, positions).

### Live view

`--view` streams the routing to a browser viewer (port 8766), `--record FILE` saves it for replay. Useful
when the user wants to watch; not needed for correctness.
