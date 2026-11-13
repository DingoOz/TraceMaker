#!/bin/bash
# M23 E3: --octilinear on tiers A and B (40 + 40 boards, seed 1) at equal work (3 M, deterministic): base, --octilinear, and
# --octilinear --octilinear-min-leg 0.5. All arms of a tier at once (4 threads per board: the E6 run shared the machine).
cd "$(dirname "$0")/../../.."
export TM_BINARY=$PWD/build/e3-tracemaker.bin
for t in A B; do
  python3 bench/run.py --tier $t --limit 40 --work 3000000 --threads 4 --jobs 2 --name e3-base-tier$t > bench/results/e3-base-tier$t.log 2>&1 &
  python3 bench/run.py --tier $t --limit 40 --work 3000000 --threads 4 --jobs 2 --name e3-oct-tier$t --route-args="--octilinear" > bench/results/e3-oct-tier$t.log 2>&1 &
  python3 bench/run.py --tier $t --limit 40 --work 3000000 --threads 4 --jobs 2 --name e3-oct05-tier$t --route-args="--octilinear --octilinear-min-leg 0.5" > bench/results/e3-oct05-tier$t.log 2>&1 &
  wait
done
echo all-done > bench/results/e3.status
