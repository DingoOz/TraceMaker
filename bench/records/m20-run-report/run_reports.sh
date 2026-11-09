#!/bin/bash
# Route the eight plane-tier boards with --report (soft zones, --si-rules, 3M work), one viewer port each.
# 2026-11-08 (D3 review): all eight in one go, interf_u included, --no-kb so the shared knowledge base neither feeds nor records the runs.
cd "$(dirname "$0")/../../.."  # the repository root
OUT=bench/results/run-report
port=8810
for d in build/demos/*/; do
  b=$(basename "$d"); b=${b// /_}
  mkdir -p "$OUT/$b"
  build/release/src/app/tracemaker route "$d/unrouted.kicad_pcb" -o "$OUT/$b/routed.kicad_pcb" --work 3000000 --time 3600 --threads 4 --soft-zones --si-rules \
    --no-kb --view --view-host 0.0.0.0 --view-port $port --report "$OUT/$b/report.html" --json "$OUT/$b/summary.json" > "$OUT/$b/log.txt" 2>&1 &
  port=$((port+1))
done
wait
for d in "$OUT"/*/; do build/release/src/app/tracemaker report "$d/summary.json" -o "$d/report.md"; done
echo all done
