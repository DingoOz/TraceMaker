#!/bin/bash
# Route the eight plane-tier boards with --report (soft zones, --si-rules, 3M work), one viewer port each.
cd "$(dirname "$0")/../../.."  # the repository root
OUT=bench/results/run-report
port=8810
for d in build/demos/*/; do
  case "$d" in *interf_u*) continue;; esac
  b=$(basename "$d"); b=${b// /_}
  mkdir -p "$OUT/$b"
  build/release/src/app/tracemaker route "$d/unrouted.kicad_pcb" -o "$OUT/$b/routed.kicad_pcb" --work 3000000 --time 3600 --threads 4 --soft-zones --si-rules \
    --view --view-port $port --report "$OUT/$b/report.html" --json "$OUT/$b/summary.json" > "$OUT/$b/log.txt" 2>&1 &
  port=$((port+1))
done
wait
echo all done
