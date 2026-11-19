#!/usr/bin/env bash
# The run behind docs/explanation/default-route.md (M25). Run from a scratch directory that holds a copy of
# tests/boards/plane_smd/plane_smd.kicad_pcb and plane_smd.kicad_pro; the engine is build/release/src/app/tracemaker.
# Default options, plus --work and --no-kb so the run gives the same board on any machine (the default stops on
# --time 120 and reads the knowledge base), plus outputs that only observe (--json, --report, --view).
set -euo pipefail
TM=${TM:-tracemaker}
"$TM" route plane_smd.kicad_pcb -o plane_smd.routed.kicad_pcb \
  --json plane_smd.json --report plane_smd.report.md \
  --no-kb --work 3000000 --view --view-port 9951 2> plane_smd.log || test $? -eq 3  # exit 3: connections left open
