#!/usr/bin/env bash
# Configure, build and test TraceMaker presets.
# Usage: scripts/bootstrap.sh [preset ...]     (default: release cpu-only asan)
set -euo pipefail
cd "$(dirname "$0")/.."
export PATH="/usr/bin:/usr/local/bin:$PATH"   # keep the Nix toolchain out of the build
presets=("$@")
[[ ${#presets[@]} -eq 0 ]] && presets=(release cpu-only asan)
status=0
for p in "${presets[@]}"; do
  echo "=== $p ==="
  cmake --preset "$p" >/dev/null
  cmake --build --preset "$p"
  if ctest --preset "$p"; then
    python3 scripts/devlog.py --kind test "Preset $p: build and tests passed"
  else
    python3 scripts/devlog.py --kind test "Preset $p: tests FAILED"
    status=1
  fi
done
exit $status
