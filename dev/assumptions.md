# Overnight decisions and assumptions (2026-10-02 → 03)

Decisions made without asking, as instructed. Each is reversible; reasons in one line.

| # | Time (UTC) | Decision | Why |
|---|---|---|---|
| A1 | 11:05 | Local git commits at each milestone or significant step (no push, no remote, no Claude attribution) | Gives you history and rollback points; nothing leaves the machine |
| A2 | 11:05 | Order of work: M1 → M2 (core DRC checks) → M4 (first router + harness) → M3 (viewer), with M3 started in parallel by a helper agent when the event schema exists | Router value first; the viewer needs routing events to be interesting |
| A3 | 11:06 | Schematic netlists come from `kicad-cli sch export netlist` for now; a native `.kicad_sch` connectivity resolver is deferred to schematic-to-board work (M7/M8) | Board files already carry pad nets; resolving schematic connectivity natively is large and not needed for routing |
| A4 | 11:06 | Ground truth for parser/geometry tests comes from KiCad's own `pcbnew` Python inside the Docker image (`scripts/kicad_truth.py`); truth JSON is committed in `tests/truth/` | Tests compare against KiCad itself, not my reading of the format |
| A5 | 11:06 | Read both KiCad 9 (`(net 3 "GND")` + net table) and KiCad 10 (`(net "GND")`) net syntax; write back in the style that was read | Demo set mixes both; writing in the read style keeps diffs minimal |
