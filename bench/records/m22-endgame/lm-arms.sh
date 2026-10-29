#!/bin/bash
cd /home/dingo/Programming/TraceMaker-speed
export TM_BINARY=$PWD/build/release/src/app/tracemaker
run() { python3 bench/run.py --set-name lm10 --boards EtherCAT_shield_v1_EtherCAT_shield_v1 kitspace_teensy-fx poncho_fpga_fpga-dongle kitspace_d20_tri_r0.3 Aleste-520EX_aleste CoreOne-xCORE200-Original_CoreOne LeeChee_1800 MonApollo_analog-board TCKB_Kicad_TCKB mini_mass_prog_bench_prog_rig --work 20000000 --time 3600 --jobs 10 --threads 4 --name lm-$1 --route-args="$2" > bench/results/lm-$1.log 2>&1; echo "$1 done" >> build/lm-arms.status; }
run base ""
run polish "--polish 0.25"
run lns "--lns-open 8"
echo all-done >> build/lm-arms.status
