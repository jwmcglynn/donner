#!/usr/bin/env bash
# The macOS remote shadow lane's routing probe (tools/ci/macos_remote_shadow.py): passes only when
# it runs on macOS arm64, so a passing, uncached remote run proves where the lane's actions go.
set -euo pipefail
expected="Darwin arm64"
actual="$(uname -s -m)"
echo "routing probe ran as: ${actual}"
if [[ "${actual}" != "${expected}" ]]; then
  echo "routing probe: expected ${expected}" >&2
  exit 1
fi
