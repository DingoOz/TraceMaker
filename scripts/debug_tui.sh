#!/usr/bin/env bash
# Launch `tracemaker tui` on a small test board with a sample options file.
#   scripts/debug_tui.sh [--build] [--plain] [BOARD]
#     --build  rebuild the release preset first
#     --plain  no --config (start from defaults)
# Output goes to a scratch directory, never next to the board.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
bin=${TM_BIN:-$root/build/release/src/app/tracemaker}
board=$root/tests/boards/plane_smd/plane_smd.kicad_pcb
conf=$root/tests/tui/debug.conf
out=${TMPDIR:-/tmp}/tm-tui-debug; mkdir -p "$out"

while [[ $# -gt 0 ]]; do
  case $1 in
    --build) (cd "$root" && cmake --preset release && cmake --build --preset release) ;;
    --plain) conf= ;;
    *) board=$1 ;;
  esac
  shift
done

[[ -x $bin ]] || { echo "no binary at $bin (try --build)" >&2; exit 1; }
args=(tui "$board" -o "$out/routed.kicad_pcb")
[[ -n $conf ]] && args+=(--config "$conf")
echo "+ $bin ${args[*]}" >&2
exec "$bin" "${args[@]}"
