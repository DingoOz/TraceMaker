#!/bin/bash
# S = a scratch directory holding o5h.kicad_pcb (built with ../scripts/inject_fanout.py box 131.78 72.56 157.28 98.06 clip, then split_oracle.py)
cd $S
r() { /home/dingo/Programming/TraceMaker-speed/build/m22-tracemaker.bin route $2 -o $1.kicad_pcb --work 20000000 --time 7200 --threads 8 --json $1.route.json --view --view-port $3 $4 > $1.log 2>&1; echo "$1 done" >> run3.status; }
rm -f run3.status
r base /home/dingo/Programming/TraceMaker-speed/bench/data/freerouting/scripts/benchmark/fixtures/PCBench/CoreOne-xCORE200-Original_CoreOne/unrouted.kicad_pcb 8861 "" &
r oh5 o5h.kicad_pcb 8862 "" &
r fh5u /home/dingo/Programming/TraceMaker-speed/bench/data/freerouting/scripts/benchmark/fixtures/PCBench/CoreOne-xCORE200-Original_CoreOne/unrouted.kicad_pcb 8863 "--fanout-heads 5 --fanout-heads-min-pins 100" &
wait; echo all-done >> run3.status
