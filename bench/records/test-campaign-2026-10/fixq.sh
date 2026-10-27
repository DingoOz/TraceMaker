#!/usr/bin/env bash
# Regression runs for the test-campaign fixes branch: quick (GPU) and planes (soft zones), compared with the 26 Oct runs.
cd /home/dingo/Programming/TraceMaker
python3 bench/run.py --name fix-g-quick --set quick --work 1000000 --view > bench/results/full-1026/fix-g-quick.log 2>&1
python3 bench/compare_runs.py full-g-quick fix-g-quick > bench/results/full-1026/compare-fix-quick.txt 2>&1
python3 bench/run.py --name fix-g-planes --fixtures build/demos --set planes --work 3000000 --time 3600 --jobs 3 --threads 8 --refill --view --route-args="--soft-zones --si-rules" > bench/results/full-1026/fix-g-planes.log 2>&1
python3 bench/compare_runs.py full-g-planes fix-g-planes > bench/results/full-1026/compare-fix-planes.txt 2>&1
echo done
