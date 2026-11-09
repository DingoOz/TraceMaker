#!/bin/bash
# staged.sh BOARD WORK MODE...: stage 1 routes everything but the power nets, stage 2 routes the board with that copper fixed.
cd /home/dingo/Programming/TraceMaker-i56
S=/tmp/claude-1000/-home-dingo-Programming-TraceMaker/a81a9786-f3ad-408b-8fd2-28bbc467d0b3/scratchpad/i56
P=bench/data/freerouting/scripts/benchmark/fixtures/PCBench
T=build/release/src/app/tracemaker
b=$1; W=$2; shift 2
mkdir -p $S/$b; cp $P/$b/unrouted.kicad_pcb $S/$b/; cp $P/$b/*.kicad_pr* $S/$b/ 2>/dev/null
PAT='GND*,*GND,VCC*,VDD*,+*,*3V3*,*5V*,*VBUS*,*VIN*'
V="--view --view-host 0.0.0.0 --view-port 8783"
[ -f $S/$b/stage1.kicad_pcb ] || $T route $S/$b/unrouted.kicad_pcb -o $S/$b/stage1.kicad_pcb --work $W --variants 1 --no-kb --no-rescue --skip-net "$PAT" --json $S/$b/stage1.json $V > $S/$b/stage1.log 2>&1
s1=$(grep "^routed" $S/$b/stage1.log | cut -d, -f1)
for m in "$@"; do
  $T route $S/$b/stage1.kicad_pcb -o $S/$b/stage2-$m.kicad_pcb --work $W --variants 1 --no-kb --no-rescue --repair $m --json $S/$b/stage2-$m.json $V > $S/$b/stage2-$m.log 2>&1
  echo "$b stage1[$s1] $m: $(grep '^routed' $S/$b/stage2-$m.log | cut -d, -f1) $(grep '^repair: [0-9]* attempts' $S/$b/stage2-$m.log)"
done
