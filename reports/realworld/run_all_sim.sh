#!/usr/bin/env bash
# transient cases and DC cases: designer's layout, our rule-ladder routes (A: plain, B/C: with critical-net rules), and the reference with the coupling gap restored to 0.2 mm
cd /home/dingo/Programming/TraceMaker/reports/realworld
T=kitspace_teensy-fx; S=Hardware_Playground_serial_gw_ATMEGA328P
{
for c in audio spi; do
  echo "$T $c human boards/$T/human/routed.kicad_pcb"
  echo "$T $c A boards/$T/ladder-A/routed.kicad_pcb"
  echo "$T $c B boards/$T/ladder-B/routed.kicad_pcb"
  echo "$T $c B boards/$T/ladder-B/routed.kicad_pcb --gap-floor 0.2"
done
echo "$S spi_usb human boards/$S/human/routed.kicad_pcb"
echo "$S spi_usb A boards/$S/ladder-A/routed.kicad_pcb"
echo "$S spi_usb A boards/$S/ladder-A/routed.kicad_pcb --gap-floor 0.2"
echo "$S spi_usb C boards/$S/ladder-C/routed.kicad_pcb"
} | xargs -P 6 -L1 ./dk.sh python3 py/run_sim.py
{
echo "$T human boards/$T/human/routed.kicad_pcb"; echo "$T A boards/$T/ladder-A/routed.kicad_pcb"; echo "$T B boards/$T/ladder-B/routed.kicad_pcb"
echo "$S human boards/$S/human/routed.kicad_pcb"; echo "$S A boards/$S/ladder-A/routed.kicad_pcb"; echo "$S C boards/$S/ladder-C/routed.kicad_pcb"
} | xargs -P 6 -L1 ./dk.sh python3 py/run_dc.py
echo sims done
