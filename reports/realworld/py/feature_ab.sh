#!/usr/bin/env bash
# feature_ab.sh BOARD RUN [route options...]: the router's count (own rules, 20 M work) with each escape feature, from the first round of a closure run.
# Prints one line per configuration: routed/connections. The router's own count, not KiCad's verdict.
set -e
cd "$(dirname "$0")/../../.."
B=reports/realworld/boards/$1; R=$2; shift 2
IN=$B/$R/stage-00-R/relaxed.kicad_pcb
X=$B/exp-features; mkdir -p $X
declare -A CFG=(
  [base]=""
  [E]="--escape-plan"
  [EL]="--escape-plan --escape-length 12"
  [ELP]="--escape-plan --escape-length 12 --escape-persist"
  [ELR]="--escape-plan --escape-length 12 --radial-supply"
  [ALL]="--escape-plan --escape-length 12 --escape-persist --radial-supply"
)
for n in "${!CFG[@]}"; do
  ( build/release/src/app/tracemaker route $IN -o $X/ab-$n.kicad_pcb --work 20000000 --threads 8 --no-gpu --no-kb --soft-zones "$@" ${CFG[$n]} > $X/ab-$n.log 2>&1 ) &
done
wait
for n in base E EL ELP ELR ALL; do printf "%s %-5s %s\n" "${1:0:0}" $n "$(grep -h '^routed' $X/ab-$n.log | awk '{print $2}')"; done
