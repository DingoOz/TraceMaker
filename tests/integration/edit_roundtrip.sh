#!/usr/bin/env bash
# Edits demo boards with TraceMaker, then checks that KiCad itself reads the edited files exactly as TraceMaker
# does (positions, rotations, pads, tracks, vias, nets). Needs Docker + the KiCad image; skips otherwise.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TM=${1:?path to tracemaker}
OUT=${2:-$ROOT/build/integration}
mkdir -p "$OUT"
cd "$ROOT"
if ! scripts/kicad-python -c "import pcbnew" >/dev/null 2>&1; then echo "SKIP: KiCad image not available"; exit 77; fi
status=0
for board in pic_programmer/pic_programmer multichannel/multichannel_mixer stickhub/StickHub; do
  src="bench/data/kicad/demos/$board.kicad_pcb"
  [[ -f $src ]] || { echo "SKIP $board (fixture missing)"; continue; }
  name=$(basename "$board")
  "$TM" selftest-edit "$src" "$OUT/$name.edited.kicad_pcb" >/dev/null
  scripts/kicad-python scripts/kicad_truth.py "$OUT/$name.edited.kicad_pcb" "$OUT/$name.kicad.json" >/dev/null
  "$TM" inspect "$OUT/$name.edited.kicad_pcb" --json "$OUT/$name.ours.json" >/dev/null
  echo -n "$name: "
  python3 scripts/compare_truth.py "$OUT/$name.kicad.json" "$OUT/$name.ours.json" || status=1
done
exit $status
