#!/bin/bash
# M23 E1: --power-width 0.5 against the base on the power-wide set at equal work (3 M, deterministic), both arms at once.
cd "$(dirname "$0")/../../.."
export TM_BINARY=$PWD/build/e1-tracemaker.bin
python3 bench/run.py --set power-wide --work 3000000 --threads 8 --jobs 4 --name e1-base > bench/results/e1-base.log 2>&1 &
python3 bench/run.py --set power-wide --work 3000000 --threads 8 --jobs 4 --name e1-pw05 --route-args="--power-width 0.5" > bench/results/e1-pw05.log 2>&1 &
wait
echo all-done > bench/results/e1.status
