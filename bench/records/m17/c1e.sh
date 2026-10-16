#!/usr/bin/env bash
# the base 8-variant arm now, beside the txn arm that is still running (idle cores)
cd /home/dingo/Programming/TraceMaker
export PATH=$PWD/scripts:$PATH TM_BINARY=$PWD/build/dist/src/app/tracemaker
python3 bench/run.py --set pcbench-bd-fail --work 100000000 --jobs 3 --threads 8 --name m17-c1-base8 --route-args=--no-gpu > bench/results/m17/c1-base8.log 2>&1
echo done > bench/results/m17/c1e.done
