#!/bin/bash
# abrun.sh NAME BINARY [extra route args...] : routes the five profile boards at 20M work, one variant, into build/ab/NAME
name=$1; bin=$2; shift 2
FIX=/home/dingo/Programming/TraceMaker/bench/data/freerouting/scripts/benchmark/fixtures/PCBench
for b in ${BOARDS:-oskirby_logicbone sbc_sbc LeeChee_1800 KiCad-Library_Teensy_test_layout kitspace_d20_r0.1}; do
  d=/home/dingo/Programming/TraceMaker-speed/build/ab/$name/$b; mkdir -p $d
  ( cd $d && /usr/bin/time -f "%U %M" $bin route "$FIX/$b/unrouted.kicad_pcb" -o out.kicad_pcb --work ${WORK:-20000000} --variants 1 --threads 1 --no-kb --no-gpu --time 3600 --seed 7 --json route.json "$@" > log.txt 2> stderr.txt ) &
done; wait
