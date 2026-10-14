#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The DRC tests items against zone fills through an edge index (geom::PolygonIndex). Its report must be
# byte-identical to the reference path, which walks every edge of every fill (drc --linear-zones), on boards with
# large fills, custom rules with area conditions and real violations, as stored and refilled. Skips when the KiCad
# demo fixtures are missing.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TM=${1:-$ROOT/build/release/src/app/tracemaker}
OUT=${2:-$ROOT/build/release/integration/drc_zone_index}
D=$ROOT/bench/data/kicad/demos
# Four boards that the linear path checks in seconds, so the test also fits a sanitizer build's time limit
# (CM5 Minima and tinytapeout, 14-16 s each on the linear path, were compared by hand: identical).
boards=("$D/video/video.kicad_pcb" "$D/stickhub/StickHub.kicad_pcb"
        "$D/kit-dev-coldfire-xilinx_5213/kit-dev-coldfire-xilinx_5213.kicad_pcb" "$D/complex_hierarchy/complex_hierarchy.kicad_pcb")
for b in "${boards[@]}"; do [[ -f $b ]] || { echo "SKIP: fixtures missing ($b)"; exit 77; }; done
mkdir -p "$OUT"
fail=0
# Also on the fills the engine's refill draws (doc 05 §36): its connectivity uses the same index.
for b in "${boards[@]}"; do
  for mode in stored refilled; do
    n=$(basename "$b" .kicad_pcb).$mode
    extra=()
    [[ $mode == refilled ]] && extra=(--refill-zones)
    "$TM" drc "$b" "${extra[@]}" --json "$OUT/$n.index.json" >/dev/null || true       # exit 5 = violations found
    "$TM" drc "$b" "${extra[@]}" --json "$OUT/$n.linear.json" --linear-zones >/dev/null || true
    if cmp -s "$OUT/$n.index.json" "$OUT/$n.linear.json"; then echo "$n: identical"; else echo "$n: reports differ"; fail=1; fi
  done
done
exit $fail
