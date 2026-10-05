#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Downloads benchmark fixture sets into bench/data (git-ignored; never redistributed).
# Usage: scripts/fetch_fixtures.sh [all|freerouting|dac2020|kicad-demos|pcbench|pcbworld] ...
# Default: freerouting dac2020 kicad-demos. Sources and licences: docs/10-benchmarking.md.
set -euo pipefail
cd "$(dirname "$0")/.."
DATA=bench/data
mkdir -p "$DATA"

sparse_clone() {  # url dir branch path...
  local url=$1 dir=$2 branch=$3; shift 3
  if [[ -d $DATA/$dir/.git ]]; then
    git -C "$DATA/$dir" pull --ff-only --quiet || true
  else
    git clone --quiet --depth 1 --filter=blob:none --sparse --branch "$branch" "$url" "$DATA/$dir"
    git -C "$DATA/$dir" sparse-checkout set "$@"
  fi
}
full_clone() {  # url dir
  if [[ -d $DATA/$2/.git ]]; then git -C "$DATA/$2" pull --ff-only --quiet || true
  else git clone --quiet --depth 1 "$1" "$DATA/$2"; fi
}

fetch() {
  case $1 in
    freerouting) sparse_clone https://github.com/freerouting/freerouting.git freerouting master fixtures scripts/benchmark ;;
    dac2020)     full_clone https://github.com/DAC-2020-Submission-1703/PCB-Benchmarks.git dac2020 ;;
    kicad-demos) sparse_clone https://gitlab.com/kicad/code/kicad.git kicad 10.0 demos ;;
    pcbench)     full_clone https://github.com/PCBench/PCBench.git pcbench ;;
    pcbworld)    full_clone https://github.com/LGAI-Research/PCBWorld.git pcbworld ;;
    *) echo "unknown set: $1"; exit 2 ;;
  esac
  echo "fetched $1 -> $DATA ($(du -sh "$DATA" | cut -f1) total)"
}

sets=("$@")
[[ ${#sets[@]} -eq 0 ]] && sets=(freerouting dac2020 kicad-demos)
[[ ${sets[0]} == all ]] && sets=(freerouting dac2020 kicad-demos pcbench pcbworld)
for s in "${sets[@]}"; do fetch "$s"; done
