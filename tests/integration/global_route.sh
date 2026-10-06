#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Global router (doc 05 §19): with corridors (--global) and with the congestion map (--global-congestion) a
# fixed-work run is repeatable byte for byte, and routing still completes the board's connections it completes
# without them to within the tolerance the corridors are known to cost. Skips when the fixture is missing.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TM=${1:-$ROOT/build/release/src/app/tracemaker}
OUT=${2:-$ROOT/build/release/integration/global_route}
SRC="$ROOT/bench/data/freerouting/scripts/benchmark/fixtures/PCBench/4-port-usb-hub_4port-usb-hub/unrouted.kicad_pcb"
[[ -f $SRC ]] || { echo "SKIP: fixture missing"; exit 77; }
mkdir -p "$OUT"
# The router exits 3 when connections stay open: not a failure here.
routed() { { "$TM" route "$SRC" -o "$1" --work 8000000 --variants 1 --threads 1 --no-kb --no-gpu "${@:2}" 2>&1 || true; } | sed -n 's/^routed \([0-9]*\)\/.*/\1/p'; }
base=$(routed "$OUT/base.kicad_pcb")
for mode in --global --global-congestion; do
  a=$(routed "$OUT/a$mode.kicad_pcb" $mode)
  b=$(routed "$OUT/b$mode.kicad_pcb" $mode)
  cmp -s "$OUT/a$mode.kicad_pcb" "$OUT/b$mode.kicad_pcb" || { echo "$mode: two runs differ"; exit 1; }
  echo "$mode: routed $a (without: $base), repeatable"
  (( a >= base - 5 )) || { echo "$mode: routed $a, more than 5 below $base"; exit 1; }
done
