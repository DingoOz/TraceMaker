#!/bin/bash
cd /home/dingo/Programming/TraceMaker-speed
export TM_BINARY=$PWD/build/release/src/app/tracemaker
B="EtherCAT_shield_v1_EtherCAT_shield_v1 kitspace_teensy-fx poncho_fpga_fpga-dongle kitspace_d20_tri_r0.3 Aleste-520EX_aleste CoreOne-xCORE200-Original_CoreOne LeeChee_1800 MonApollo_analog-board TCKB_Kicad_TCKB mini_mass_prog_bench_prog_rig"
run() { python3 bench/run.py --set-name lm10 --boards $B --work $2 --time 7200 --jobs 10 --threads 4 --name lm-$1 --route-args="$3" > bench/results/lm-$1.log 2>&1; echo "$1 done" >> build/lm-arms3.status; }
rm -f build/lm-arms3.status
run eg10b 20000000 "--endgame 0.1" &
run eg10rb 20000000 "--endgame 0.1 --rescue 2,4,8" &
wait
echo all-done >> build/lm-arms3.status
