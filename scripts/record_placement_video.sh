#!/bin/bash
set -e
cd "$(dirname "$0")/.."
V=build/video_place
B=${1:-retroreflectors_SALSAFLOCK}
mkdir -p $V
cp bench/data/freerouting/scripts/benchmark/fixtures/PCBench/$B/unrouted.kicad_pcb $V/human.kicad_pcb
python3 bench/cpu_sample.py $V/cpu_place.json --gpu -- build/release/src/place/tracemaker-place $V/human.kicad_pcb -o $V/placed.kicad_pcb --mode routable --route-check 3000000 --threads 8 --json $V/placed.json --record $V/place_events.jsonl > $V/place.log 2>&1
python3 bench/cpu_sample.py $V/cpu_route.json --gpu -- build/release/src/app/tracemaker route $V/placed.kicad_pcb -o $V/tm.kicad_pcb --time 120 --threads 8 --no-kb --record $V/tm_events.jsonl > $V/tm_route.log 2>&1
python3 bench/cpu_sample.py $V/cpu_fr.json -- build/report-venv/bin/python bench/record_fr.py $B 2.5.0-RC12 $V/fr_human.mp4 --display 97 > $V/rec_human.log 2>&1
python3 bench/dsn_place.py $V/human.kicad_pcb bench/data/freerouting/scripts/benchmark/fixtures/PCBench/$B/unrouted.dsn $V/placed.kicad_pcb -o $V/placed.dsn
build/report-venv/bin/python bench/record_fr.py $B 2.5.0-RC12 $V/fr_placed.mp4 --display 98 --dsn $V/placed.dsn > $V/rec_placed.log 2>&1
# Control: TraceMaker's router on the designer's placement (same settings).
build/release/src/app/tracemaker route $V/human.kicad_pcb -o $V/tm_human.kicad_pcb --time 120 --threads 8 --no-kb > $V/tm_human.log 2>&1 || true  # exits non-zero when connections stay unrouted
echo ALL DONE
