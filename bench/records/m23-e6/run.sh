#!/bin/bash
# M23 E6: one time limit for the whole run, at equal wall time, on the 33 failing tier B-D boards (all three arms at once).
#   A: today, --time 40 (each stage gets 40 s: about 120 s on a board left open)
#   B: --time-total 120 (the plain run 60 s, the rescue passes what is left)
#   C: --time-total 120 --rescue-share 0.75 (the plain run 90 s)
cd "$(dirname "$0")/../../.."
export TM_BINARY=$PWD/build/e6-tracemaker.bin
python3 bench/run.py --set pcbench-bd-fail --time 40 --threads 8 --jobs 2 --name e6-a-time40 > bench/results/e6-a.log 2>&1 &
python3 bench/run.py --set pcbench-bd-fail --time 120 --threads 8 --jobs 2 --name e6-b-total120 --route-args="--time-total 120" > bench/results/e6-b.log 2>&1 &
python3 bench/run.py --set pcbench-bd-fail --time 120 --threads 8 --jobs 2 --name e6-c-total120-s75 --route-args="--time-total 120 --rescue-share 0.75" > bench/results/e6-c.log 2>&1 &
wait
echo all-done > bench/results/e6.status
