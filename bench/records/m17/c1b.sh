#!/usr/bin/env bash
cd /home/dingo/Programming/TraceMaker
export PATH=$PWD/scripts:$PATH TM_BINARY=$PWD/build/dist/src/app/tracemaker
python3 bench/run.py --set pcbench-bd-fail --work 100000000 --jobs 12 --threads 1 --name m17-c1-relax1 --route-args "--variants 1 --no-gpu --relax --round-work 20000000 --rounds 5" > bench/results/m17/c1-relax1.log 2>&1
echo done > bench/results/m17/c1b.done
