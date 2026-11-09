#!/bin/bash
# Issue #66 comparison runs on relaymini (one after the other, viewer on port 8785).
set -u
TM=${TM:-build/release/src/app/tracemaker}
cd "$(dirname "$0")"
W="--work 3000000 --variants 2 --threads 2 --no-gpu --no-kb --time 3600 --view --view-host 0.0.0.0 --view-port 8785 --no-freeze"
run() { name=$1; shift; cp relaymini.kicad_pro $name.kicad_pro; "$TM" route relaymini.kicad_pcb -o $name.kicad_pcb $W "$@" > $name.log 2>&1; echo "$name rc=$? $(grep -E '^routed' $name.log | head -1)"; }
for spec in "$@"; do eval "run $spec"; done
