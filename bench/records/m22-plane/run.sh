#!/bin/bash
cd /home/dingo/Programming/TraceMaker-rm3
export TM_BINARY=$PWD/build/m22p-tracemaker.bin
run() { python3 bench/run.py --set pcbench-bd-fail --work 10000000 --time 7200 --jobs 7 --threads 4 --name $1 --route-args="$2" > bench/results/$1.log 2>&1; echo "$1 done $?" >> bench/results/m22p.status; }
rm -f bench/results/m22p.status
run m22p-base "" &
run m22p-plane "--plane-informed" &
wait
echo all-done >> bench/results/m22p.status
