#!/bin/bash
S=/tmp/claude-1000/-home-dingo-Programming-TraceMaker/a81a9786-f3ad-408b-8fd2-28bbc467d0b3/scratchpad/i55
cd $S/poe
for f in before after so2; do
  /usr/bin/time -f "%e s %M KB" $S/new/tracemaker route $f-full.kicad_pcb -o $f-routed.kicad_pcb --diff-pairs --work 10000000 --time 20000 --threads 8 --no-kb \
    --json $f-route.json --stuck-report $f-stuck.json --view --view-host 0.0.0.0 --view-port 8782 > $f-route.log 2>&1
  echo "$f exit $?"
done
