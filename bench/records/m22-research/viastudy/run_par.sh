#!/bin/bash
# Runs viastudy on several boards at once: JOBS boards in parallel, THREADS router threads each (JOBS*THREADS <= 24).
# Each argument: "tier board max_nets [experiments] [output-suffix] [binary]".
cd /home/dingo/Programming/TraceMaker-speed
S=build/via-layer-study
F=bench/data/freerouting/scripts/benchmark/fixtures/PCBench
one() {
  set -- $1
  local R=bench/results/rel0120-tier$1/boards exps=${4:-A,V05,V1,V2,T2,VT2,BIG,CHEAP,FINE,SWAP1} suf=${5:-} bin=${6:-viastudy}
  echo "== start $2$suf $(date +%T) $exps"
  /usr/bin/time -f "%e s wall %U s user" $S/$bin $R/$2.kicad_pcb $F/$2/unrouted.kicad_pcb $S/out/$2$suf.json $THREADS $3 $exps > $S/out/$2$suf.log 2>&1
  echo "== end $2$suf $(date +%T) $(tail -1 $S/out/$2$suf.log)"
}
export -f one; export S F THREADS
printf '%s\n' "$@" | xargs -P ${JOBS:-3} -I{} bash -c 'one "{}"'
echo "== all done $(date +%T)"
