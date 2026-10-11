#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Downloads benchmark fixture sets into bench/data (git-ignored; never redistributed) and builds the sets derived
# from them.
# Usage: scripts/fetch_fixtures.sh [all|freerouting|dac2020|kicad-demos|pcbench|pcbworld|derived|tscircuit|tscircuit-checker] ...
# Default: freerouting dac2020 kicad-demos derived. Sources and licences: docs/10-benchmarking.md.
#
# The three default sets are pinned to the commits the published results were measured on (release 0.8.0), so a
# fresh clone gets the same boards, tiers and Freerouting result file. To take upstream's current state instead:
#   TM_FIXTURES=latest scripts/fetch_fixtures.sh
# or one set: TM_REF_FREEROUTING=<commit|latest>, TM_REF_DAC2020=..., TM_REF_KICAD=...
set -euo pipefail
cd "$(dirname "$0")/.."
DATA=bench/data
mkdir -p "$DATA"

PIN_FREEROUTING=d216f906bb8dd6aff21d2c5114db194d08bd1a0f   # freerouting master, 2026-09-30
PIN_DAC2020=bee28883cddc260d91d6e40e38f312f8293d6a6c
PIN_KICAD=7a7d0267a0bcf7cff2e3d7c6a030b602f4c88a8f         # KiCad branch 10.0
# tscircuit's Simple Route JSON BGA breakout problems (doc 10 §7) and, for its "relaxed DRC" checker, its autorouter.
PIN_SRJ19=c1206f3901d7c2d626733f7937e00b753e15f5ed          # tscircuit/dataset-srj19 main, 2026-06-06 (MIT)
PIN_SRJ20=9384fb8f45fb479b97178ddfa42fc74c69afbbec          # tscircuit/dataset-srj20 main, 2026-06-07 (no licence file)
PIN_TSC_AUTOROUTER=5bcf7747433f28d24cad9c9f4aa417838dab88fb # tscircuit/tscircuit-autorouter main, 2026-10-10 (MIT)

ref_for() {  # NAME pin -> the commit to use, or "latest"
  local var=TM_REF_$1
  if [[ -n ${!var:-} ]]; then echo "${!var}"; elif [[ ${TM_FIXTURES:-} == latest ]]; then echo latest; else echo "$2"; fi
}

# Puts the checkout at `ref` (a commit: fetched on its own, shallow) or, for "latest", at the branch head.
pin_checkout() {  # dir ref
  local dir=$DATA/$1 ref=$2
  if [[ $ref == latest ]]; then
    git -C "$dir" pull --ff-only --quiet || true
  elif [[ $(git -C "$dir" rev-parse HEAD 2>/dev/null) != "$ref" ]]; then
    git -C "$dir" fetch --quiet --depth 1 origin "$ref"
    git -C "$dir" checkout --quiet --detach "$ref"
  fi
}
sparse_clone() {  # url dir branch ref path...
  local url=$1 dir=$2 branch=$3 ref=$4; shift 4
  if [[ ! -d $DATA/$dir/.git ]]; then
    git clone --quiet --depth 1 --filter=blob:none --sparse --no-checkout --branch "$branch" "$url" "$DATA/$dir"
    git -C "$DATA/$dir" sparse-checkout set "$@"
    [[ $ref == latest ]] && git -C "$DATA/$dir" checkout --quiet "$branch"
  fi
  pin_checkout "$dir" "$ref"
}
full_clone() {  # url dir [ref]
  if [[ ! -d $DATA/$2/.git ]]; then git clone --quiet --depth 1 "$1" "$DATA/$2"; fi
  pin_checkout "$2" "${3:-latest}"
}

# Sets built from the downloads: the DAC 2020 boards in the PCBench layout (needs freerouting + dac2020), and the
# from-scratch placement set S (needs PCBench, bench/place_sets/H.txt and a built tracemaker-place).
derived() {
  if [[ -d $DATA/dac2020 && -d $DATA/freerouting/scripts/benchmark/fixtures/DAC2020_boards ]]; then
    python3 bench/prepare_dac2020.py > /dev/null && echo "built $DATA/dac2020_prepared"
  else
    echo "skipped dac2020_prepared: fetch freerouting and dac2020 first"
  fi
  if [[ ! -x build/release/src/place/tracemaker-place ]]; then
    echo "skipped placement set S: build first (cmake --preset release && cmake --build --preset release), then run: scripts/fetch_fixtures.sh derived"
  elif [[ ! -d $DATA/freerouting/scripts/benchmark/fixtures/PCBench ]]; then
    echo "skipped placement set S: fetch freerouting first"
  else
    python3 bench/make_place_sets.py scratch
  fi
}

# tscircuit's own checker: the autorouter's lib/ (its evaluateRelaxedDrc) and the npm packages it imports, at the
# versions the autorouter's package.json names, in bench/data/tscircuit (bench/srj_score.py runs it with node).
tscircuit_checker() {
  sparse_clone https://github.com/tscircuit/tscircuit-autorouter.git tscircuit/tscircuit-autorouter main "$(ref_for TSC_AUTOROUTER $PIN_TSC_AUTOROUTER)" lib types
  # sparse_clone leaves the work tree empty when the branch head already is the pinned commit.
  [[ -d $DATA/tscircuit/tscircuit-autorouter/lib ]] || git -C "$DATA/tscircuit/tscircuit-autorouter" checkout --quiet --detach HEAD
  cat > "$DATA/tscircuit/package.json" <<'JSON'
{
  "name": "tracemaker-srj-checker",
  "private": true,
  "type": "module",
  "dependencies": {
    "@flatten-js/core": "1.6.14",
    "@tscircuit/checks": "0.0.233",
    "@tscircuit/circuit-json-util": "0.0.114",
    "@tscircuit/math-utils": "0.0.36",
    "@tscircuit/solver-utils": "0.0.16",
    "circuit-json": "0.0.506",
    "circuit-json-to-connectivity-map": "0.0.19",
    "graphics-debug": "0.0.99",
    "high-density-repair03": "https://codeload.github.com/tscircuit/high-density-repair03/tar.gz/e16df6c48efed49406cf45df49fd933d3141973d",
    "tsx": "4.23.15"
  }
}
JSON
  (cd "$DATA/tscircuit" && npm install --ignore-scripts --no-audit --no-fund --silent)
}

fetch() {
  case $1 in
    freerouting) sparse_clone https://github.com/freerouting/freerouting.git freerouting master "$(ref_for FREEROUTING $PIN_FREEROUTING)" fixtures scripts/benchmark ;;
    dac2020)     full_clone https://github.com/DAC-2020-Submission-1703/PCB-Benchmarks.git dac2020 "$(ref_for DAC2020 $PIN_DAC2020)" ;;
    kicad-demos) sparse_clone https://gitlab.com/kicad/code/kicad.git kicad 10.0 "$(ref_for KICAD $PIN_KICAD)" demos ;;
    pcbench)     full_clone https://github.com/PCBench/PCBench.git pcbench ;;
    pcbworld)    full_clone https://github.com/LGAI-Research/PCBWorld.git pcbworld ;;
    derived)     derived; return ;;
    tscircuit)
      full_clone https://github.com/tscircuit/dataset-srj19.git tscircuit/dataset-srj19 "$(ref_for SRJ19 $PIN_SRJ19)"
      full_clone https://github.com/tscircuit/dataset-srj20.git tscircuit/dataset-srj20 "$(ref_for SRJ20 $PIN_SRJ20)"
      if [[ -x build/release/src/app/tracemaker ]]; then python3 bench/prepare_srj.py; else echo "skipped the srj19/srj20 boards: build first, then run: python3 bench/prepare_srj.py"; fi ;;
    tscircuit-checker) tscircuit_checker ;;
    *) echo "unknown set: $1"; exit 2 ;;
  esac
  local at=""
  [[ -d $DATA/${1/kicad-demos/kicad}/.git ]] && at=" at $(git -C "$DATA/${1/kicad-demos/kicad}" rev-parse --short HEAD)"
  echo "fetched $1$at -> $DATA ($(du -sh "$DATA" | cut -f1) total)"
}

sets=("$@")
[[ ${#sets[@]} -eq 0 ]] && sets=(freerouting dac2020 kicad-demos derived)
[[ ${sets[0]} == all ]] && sets=(freerouting dac2020 kicad-demos pcbench pcbworld derived)
for s in "${sets[@]}"; do fetch "$s"; done
