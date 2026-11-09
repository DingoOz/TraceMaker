#!/bin/bash
# arm.sh BOARD NAME WORK REPAIR [extra...]: stage 2 of the staged flow under one setting.
cd /home/dingo/Programming/TraceMaker-i56
S=/tmp/claude-1000/-home-dingo-Programming-TraceMaker/a81a9786-f3ad-408b-8fd2-28bbc467d0b3/scratchpad/i56
T=build/release/src/app/tracemaker
b=$1; n=$2; W=$3; m=$4; shift 4
$T route $S/$b/stage1.kicad_pcb -o $S/$b/s2-$n.kicad_pcb --work $W --variants 1 --no-kb ${RESCUE---no-rescue} --repair $m --json $S/$b/s2-$n.json "$@" --view --view-host 0.0.0.0 --view-port 8783 > $S/$b/s2-$n.log 2>&1
echo "$b $n: $(grep '^routed' $S/$b/s2-$n.log | cut -d, -f1) $(grep '^repair: [0-9]* attempts' $S/$b/s2-$n.log)"
