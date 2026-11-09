#!/bin/bash
# M21 A3: the Kookabus tier in three arms (D176). Needs read access to the private DingoOz/kookabus repository.
set -e
cd "$(dirname "$0")/../../.."  # the repository root
export KICAD_CLI=${KICAD_CLI:-$PWD/scripts/kicad-cli}
scripts/fetch_fixtures.sh kookabus
python3 bench/prepare_demos.py --out build/kookabus --set kookabus
for arm in "base:--soft-zones" "si:--soft-zones --si-rules" "pairs:--soft-zones --si-rules --diff-pairs"; do
  python3 bench/run.py --fixtures build/kookabus --set kookabus --work 3000000 --refill --jobs 6 --threads 4 \
    --route-args="${arm#*:}" --name "kookabus-${arm%%:*}"
done
# The designer's layouts under the same judge.
for p in bench/data/kookabus/kookabus-0{1,2,3,4,6}/*.kicad_pcb bench/data/kookabus/kookabus-04h/*.kicad_pcb; do
  "$KICAD_CLI" pcb drc --refill-zones --schematic-parity --format json --severity-error -o "$PWD/bench/results/$(basename "$p" .kicad_pcb).designer.drc.json" "$PWD/$p"
done
