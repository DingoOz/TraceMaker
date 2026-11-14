#!/bin/bash
# M23 E5: is completion on the failing boards limited by search volume? 33 pcbench-bd-fail boards, no rescue, 5 M against 40 M work
# (deterministic), both arms at once, 3 jobs x 8 threads each.
cd "$(dirname "$0")/../../.."
if [ -z "$1" ]; then
export TM_BINARY=$PWD/build/e5-tracemaker.bin
python3 bench/run.py --set pcbench-bd-fail --work 5000000 --threads 8 --jobs 3 --name e5-work5m --route-args="--no-rescue" > bench/results/e5-work5m.log 2>&1 &
python3 bench/run.py --set pcbench-bd-fail --work 40000000 --threads 8 --jobs 3 --name e5-work40m --route-args="--no-rescue" > bench/results/e5-work40m.log 2>&1 &
wait
echo all-done > bench/results/e5.status
fi

# Part 2 (after the row-skipping CPU field, doc 05 §83): equal wall time, old binary (build/e5-tracemaker.bin) against new
# (build/e5f-tracemaker.bin), --time-total 120, the 33 boards, both at once, 2 jobs x 8 threads each.
if [ "$1" = "time" ]; then
  TM_BINARY=$PWD/build/e5-tracemaker.bin python3 bench/run.py --set pcbench-bd-fail --time 120 --threads 8 --jobs 2 --name e5t-old --route-args="--time-total 120" > bench/results/e5t-old.log 2>&1 &
  TM_BINARY=$PWD/build/e5f-tracemaker.bin python3 bench/run.py --set pcbench-bd-fail --time 120 --threads 8 --jobs 2 --name e5t-new --route-args="--time-total 120" > bench/results/e5t-new.log 2>&1 &
  wait
  echo time-done > bench/results/e5t.status
fi

# Part 3: as part 2 with --no-gpu (a machine without a CUDA GPU, such as most plugin users').
if [ "$1" = "time-cpu" ]; then
  TM_BINARY=$PWD/build/e5-tracemaker.bin python3 bench/run.py --set pcbench-bd-fail --time 120 --threads 8 --jobs 2 --name e5c-old --route-args="--time-total 120 --no-gpu" > bench/results/e5c-old.log 2>&1 &
  TM_BINARY=$PWD/build/e5f-tracemaker.bin python3 bench/run.py --set pcbench-bd-fail --time 120 --threads 8 --jobs 2 --name e5c-new --route-args="--time-total 120 --no-gpu" > bench/results/e5c-new.log 2>&1 &
  wait
  echo cpu-done > bench/results/e5c.status
fi
