#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Broken-board DRC parity regression: defects injected deterministically into the boards listed in
# drc_broken_boards.txt (shorts, crossings, dangling tracks and vias, cut connections; see
# `tracemaker selftest-defects`) must be classified exactly like kicad-cli does, violation by violation.
# Two seeds, 12 defect kinds, 3 defects each. Needs kicad-cli (Docker image); skips otherwise. KiCad results are
# cached by file content in build/drc/kicad, so only the first run calls kicad-cli (at most 2 containers at once).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
command -v kicad-cli >/dev/null || { echo "SKIP: kicad-cli missing"; exit 77; }
mapfile -t boards < <(grep -v '^#' tests/integration/drc_broken_boards.txt)
for b in "${boards[@]}"; do [[ -f $b ]] || { echo "SKIP: fixtures missing ($b)"; exit 77; }; done
status=0
for seed in 1 2; do
  python3 scripts/drc_broken_parity.py --kicad-jobs 2 --jobs 4 --count 3 --seed "$seed" "${boards[@]}" || status=1
done
exit $status
