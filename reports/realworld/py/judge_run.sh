#!/usr/bin/env bash
# judge_run.sh BOARD[+planes] RUN LAYERS: compromise ledger (KiCad) and safety simulation of an already routed run (closure.py or ladder.py output)
set -e
cd "$(dirname "$0")/.."
B=${1%+planes}; R=$2; L=$3
D=boards/$B/$R
BASE=boards/$B/planes.kicad_pcb; [ -f $D/baseline.kicad_pcb ] && BASE=$D/baseline.kicad_pcb
python3 py/ledger.py $D/routed.kicad_pcb $BASE $D/ledger.json --layers $L > $D/ledger.log 2>&1
./dk.sh python3 py/safety.py $B $R
