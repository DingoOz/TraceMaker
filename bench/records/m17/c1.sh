#!/usr/bin/env bash
# C1: --txn and --relax on the 33 failing B-D boards, one variant, 100 M work, one thread per board.
cd /home/dingo/Programming/TraceMaker
export PATH=$PWD/scripts:$PATH TM_BINARY=$PWD/build/dist/src/app/tracemaker
python3 bench/run.py --set pcbench-bd-fail --work 100000000 --jobs 12 --threads 1 --name m17-c1-base  --route-args "--variants 1 --no-gpu" > bench/results/m17/c1-base.log 2>&1
python3 bench/run.py --set pcbench-bd-fail --work 100000000 --jobs 12 --threads 1 --name m17-c1-txn   --route-args "--variants 1 --no-gpu --txn 1" > bench/results/m17/c1-txn.log 2>&1
python3 bench/run.py --set pcbench-bd-fail --work 100000000 --jobs 12 --threads 1 --name m17-c1-relax --route-args "--variants 1 --no-gpu --relax --round-work 20000000 --rounds 5" > bench/results/m17/c1-relax.log 2>&1
echo done > bench/results/m17/c1.done
