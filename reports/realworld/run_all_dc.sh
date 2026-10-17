#!/usr/bin/env bash
cd /home/dingo/Programming/TraceMaker/reports/realworld
T=kitspace_teensy-fx; S=Hardware_Playground_serial_gw_ATMEGA328P
printf '%s\n' "$T human boards/$T/human/routed.kicad_pcb" "$T ours boards/$T/ladder-A/routed.kicad_pcb" "$S human boards/$S/human/routed.kicad_pcb" "$S ours boards/$S/ladder-A/routed.kicad_pcb" | xargs -P 4 -L1 ./dk.sh python3 py/run_dc.py
