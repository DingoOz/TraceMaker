#!/bin/bash
# issue #67 measurement (run_all.sh BINARY OUT JOBS NAME=ARGS ...): 18 PCBench pair boards + 2 KiCad demos (with --soft-zones), 30 M work, one variant.
set -u
cd "$(dirname "$0")/../../.."
mapfile -t ALL < bench/records/issue67/boards.txt
PCB=("${ALL[@]:0:18}"); DEMO=("${ALL[@]:18}")
BIN=$1; OUT=$2; JOBS=$3; shift 3
MODES=(); DMODES=()
for m in "$@"; do MODES+=(--mode "$m"); DMODES+=(--mode "$m --soft-zones"); done
python3 bench/pair_eval.py "${PCB[@]}" --tm $BIN --jobs $JOBS --drc-jobs 2 --out build/i67/$OUT "${MODES[@]}" --json build/i67/$OUT-pcbench.json > build/i67/$OUT-pcbench.txt 2>&1
python3 bench/pair_eval.py "${DEMO[@]}" --tm $BIN --jobs $JOBS --drc-jobs 2 --out build/i67/$OUT "${DMODES[@]}" --json build/i67/$OUT-demos.json > build/i67/$OUT-demos.txt 2>&1
echo finished $OUT
