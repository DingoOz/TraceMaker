#!/usr/bin/env bash
# C1 with the default eight-variant portfolio: 100 M work per variant (relax: 5 rounds of 20 M), 8 threads per board, 6 boards at a time.
cd /home/dingo/Programming/TraceMaker
export PATH=$PWD/scripts:$PATH TM_BINARY=$PWD/build/dist/src/app/tracemaker
python3 bench/run.py --set pcbench-bd-fail --work 100000000 --jobs 6 --threads 8 --name m17-c1-base8  --route-args "--no-gpu" > bench/results/m17/c1-base8.log 2>&1
python3 bench/run.py --set pcbench-bd-fail --work 100000000 --jobs 6 --threads 8 --name m17-c1-txn8   --route-args "--no-gpu --txn 1" > bench/results/m17/c1-txn8.log 2>&1
python3 bench/run.py --set pcbench-bd-fail --work 100000000 --jobs 6 --threads 8 --name m17-c1-relax8 --route-args "--no-gpu --relax --round-work 20000000 --rounds 5" > bench/results/m17/c1-relax8.log 2>&1
echo done > bench/results/m17/c1c.done
