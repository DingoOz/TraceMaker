#!/bin/bash
# Ten failing boards with the fan-out heads (D157), 8 variants, 20 M work, as m22-all.sh.
cd /home/dingo/Programming/TraceMaker-speed
export TM_BINARY=$PWD/build/m22-tracemaker.bin
unset TM_PICK_VARIANTS TM_DEBUG_SHOVE TM_DEBUG_CONN TM_DEBUG_HEADS
B="EtherCAT_shield_v1_EtherCAT_shield_v1 kitspace_teensy-fx poncho_fpga_fpga-dongle kitspace_d20_tri_r0.3 Aleste-520EX_aleste CoreOne-xCORE200-Original_CoreOne LeeChee_1800 MonApollo_analog-board TCKB_Kicad_TCKB mini_mass_prog_bench_prog_rig"
run() { python3 bench/run.py --set-name lm10 --boards $B --work $2 --time 7200 --jobs 10 --threads 4 --name lm-$1 --route-args="$3" > bench/results/lm-$1.log 2>&1; echo "$1 done" >> build/m22-heads.status; }
rm -f build/m22-heads.status
run m22heads 20000000 "--fanout-heads 5" &
run m22headsse 20000000 "--fanout-heads 5 --via-shove --escape-all" &
wait
echo all-done >> build/m22-heads.status
