#!/bin/bash
cd /home/dingo/Programming/TraceMaker-speed
BIN=$PWD/build/m22-attr.bin; D=bench/data/freerouting/scripts/benchmark/fixtures/PCBench; O=build/m22-attr; mkdir -p $O
unset TM_PICK_VARIANTS TM_DEBUG_SHOVE TM_DEBUG_CONN
run() { port=$1; b=$2; n=$3; shift 3; $BIN route $D/$b/unrouted.kicad_pcb --output $O/$b.$n.kicad_pcb --time 3600 --work 20000000 --no-kb --threads 4 --view --view-port $port "$@" --json $O/$b.$n.route.json > $O/$b.$n.log 2>&1; echo "$b $n" >> $O/status; }
rm -f $O/status $O/*.log
run 8851 kitspace_d20_tri_r0.3 shove --via-shove & run 8852 kitspace_d20_tri_r0.3 eall --escape-all &
run 8853 poncho_fpga_fpga-dongle shove --via-shove & run 8854 poncho_fpga_fpga-dongle pvp --pin-via-pen 3 & run 8855 poncho_fpga_fpga-dongle eall --escape-all &
a=Aleste-520EX_aleste
run 8856 $a wesc --window-escalate & run 8857 $a vretry --via-retry & run 8858 $a pbound --pass-bound & run 8859 $a nscap --neg-strict-cap 200000 & run 8860 $a lw --long-weight 2 & run 8861 $a plane --plane-informed &
wait; echo all-done >> $O/status
