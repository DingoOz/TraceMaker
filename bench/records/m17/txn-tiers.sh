#!/bin/bash
# M17: --txn 1 against the default at equal wall time on tiers A-D (the release sets: --limit 40 / 40 / 30 / 40, seed 1, 120 s, 8 threads).
# Both arms run at once with the same jobs, so the machine's load is the same for both.
cd "$(dirname "$0")/../../.."
export TM_BINARY=$PWD/build/txn-tracemaker.bin
arm() {  # name, extra route args
  for t in A B C D; do
    lim=40; [ $t = C ] && lim=30
    python3 bench/run.py --tier $t --limit $lim --time 120 --threads 8 --jobs 3 --name txn-$1-tier$t --route-args="$2" > bench/results/txn-$1-tier$t.log 2>&1
    echo "$1 $t done $?" >> bench/results/txn-tiers.status
  done
}
rm -f bench/results/txn-tiers.status
arm base "" &
arm txn "--txn 1" &
wait
echo all-done >> bench/results/txn-tiers.status
