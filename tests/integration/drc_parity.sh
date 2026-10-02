#!/usr/bin/env bash
# DRC parity regression: TraceMaker's DRC must keep matching kicad-cli exactly (15 routing-relevant types) on
# every board listed in drc_parity_boards.txt. Needs kicad-cli (Docker image); skips otherwise.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
command -v kicad-cli >/dev/null || { echo "SKIP: kicad-cli missing"; exit 77; }
mapfile -t boards < <(grep -v '^#' tests/integration/drc_parity_boards.txt)
for b in "${boards[@]}"; do [[ -f $b ]] || { echo "SKIP: fixtures missing ($b)"; exit 77; }; done
python3 scripts/drc_parity.py --jobs 8 "${boards[@]}"
