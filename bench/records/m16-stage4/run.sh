#!/bin/bash
cd bench/records/m16-stage4
for b in fan_controller_fan_controller esp8266_envmonitor_environment-monitor-1.2 spisolator_spisolator; do
  ./place.bin $b.kicad_pcb -o $b.adv.kicad_pcb --mode routable --route-check 1000000 --route-threads 4 --rounds 1 --eco-candidates 2 --final-work 0 --place-advice $b.json --json $b.adv.json > $b.adv.log 2>&1 &
  ./place.bin $b.kicad_pcb -o $b.ctl.kicad_pcb --mode routable --route-check 1000000 --route-threads 4 --rounds 1 --eco-candidates 2 --final-work 0 --json $b.ctl.json > $b.ctl.log 2>&1 &
done
wait
echo done > oracle.done
