# Overnight decisions and assumptions (2026-10-02 → 03)

Decisions made without asking, as instructed. Each is reversible; reasons in one line.

| # | Time (UTC) | Decision | Why |
|---|---|---|---|
| A1 | 11:05 | Local git commits at each milestone or significant step (no push, no remote, no Claude attribution) | Gives you history and rollback points; nothing leaves the machine |
| A2 | 11:05 | Order of work: M1 → M2 (core DRC checks) → M4 (first router + harness) → M3 (viewer), with M3 started in parallel by a helper agent when the event schema exists | Router value first; the viewer needs routing events to be interesting |
| A3 | 11:06 | Schematic netlists come from `kicad-cli sch export netlist` for now; a native `.kicad_sch` connectivity resolver is deferred to schematic-to-board work (M7/M8) | Board files already carry pad nets; resolving schematic connectivity natively is large and not needed for routing |
| A4 | 11:06 | Ground truth for parser/geometry tests comes from KiCad's own `pcbnew` Python inside the Docker image (`scripts/kicad_truth.py`); truth JSON is committed in `tests/truth/` | Tests compare against KiCad itself, not my reading of the format |
| A5 | 11:06 | Read both KiCad 9 (`(net 3 "GND")` + net table) and KiCad 10 (`(net "GND")`) net syntax; write back in the style that was read | Demo set mixes both; writing in the read style keeps diffs minimal |
| A6 | 11:20 | Repository-local git identity: name `dingo`, email from your Claude account. Global git config untouched | Commits need an identity; change with `git config user.name` |
| A7 | 11:25 | Footprint flipping (top↔bottom) is not implemented in the M1 editor; it comes with placement (M7), where side changes are needed | Flipping mirrors every child item and swaps layers; not needed for routing |
| A8 | 11:25 | Zone and pad-primitive arcs are flattened to their start/mid/end points for now; exact arc geometry comes with the M2 geometry kernel | Enough for reading; DRC needs exact shapes |
| A9 | 11:25 | Effective net class = explicit assignment, else first matching pattern, else Default (KiCad 9+ can combine several classes by priority; not modelled yet) | Matches all demo projects; multi-class merging is rare |
