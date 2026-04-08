#!/bin/bash
#
# Local presubmit checks that mirror the CI pipeline.
#
# Most lints are wired into `bazel test //...` directly (via the
# `donner_cc_{library,test,binary}` macros in build_defs/rules.bzl), so
# `bazel test //...` catches banned source patterns automatically. This
# script adds the two checks that can't live inside bazel test today:
#
#   - `gen_cmakelists.py --check` (can't run bazel query inside bazel test)
#   - clang-format on modified files (needs the local `clang-format` binary)
#
# Run before opening a PR. Designed to be fast (<2 min for lints) and
# comprehensive for the test step.
#
# Usage:
#   tools/presubmit.sh                 # Run all checks
#   tools/presubmit.sh --fast          # Skip slow checks (bazel test)
#   tools/presubmit.sh --no-cmake      # Skip CMake validation
#   tools/presubmit.sh --help          # Show usage

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPO_ROOT}"

FAST=0
RUN_CMAKE=1
RUN_BAZEL=1
RUN_FORMAT=1

while [[ $# -gt 0 ]]; do
  case "$1" in
    --fast) FAST=1; RUN_BAZEL=0; shift ;;
    --no-cmake) RUN_CMAKE=0; shift ;;
    --no-bazel) RUN_BAZEL=0; shift ;;
    --no-format) RUN_FORMAT=0; shift ;;
    --help|-h)
      sed -n '3,22p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *) echo "Unknown argument: $1" >&2; exit 2 ;;
  esac
done

declare -a FAILED_CHECKS=()

run_check() {
  local name="$1"; shift
  echo "===[ ${name} ]==="
  if "$@"; then
    echo "  ok"
  else
    echo "  FAILED"
    FAILED_CHECKS+=("${name}")
  fi
  return 0  # Always continue
}

# 1. CMake generation validation (generator bugs + output integrity).
#    Runs outside bazel because gen_cmakelists.py uses bazel query which
#    can't be reentrantly invoked from inside bazel test. See PR #3 / the
#    aspect design doc for the plan to bring this into bazel natively.
if [[ "${RUN_CMAKE}" == "1" ]]; then
  run_check "cmake validation" python3 tools/cmake/gen_cmakelists.py --check
fi

# 2. `bazel test //...` — runs all unit tests AND all auto-generated lint
#    tests (banned_patterns per-library). A new `long long` or UDL operator
#    in a source file now fails here automatically.
if [[ "${RUN_BAZEL}" == "1" ]]; then
  run_check "bazel test //..." bazel test //...
fi

# 3. clang-format check on modified files (fast, only local).
if [[ "${RUN_FORMAT}" == "1" ]] && command -v clang-format >/dev/null 2>&1; then
  MODIFIED_SOURCES="$(git diff --name-only HEAD 2>/dev/null \
    | grep -E '\.(cc|h|hpp|cpp)$' \
    | grep -v '^third_party/' || true)"
  if [[ -n "${MODIFIED_SOURCES}" ]]; then
    run_check "clang-format" bash -c \
      "echo '${MODIFIED_SOURCES}' | xargs clang-format --dry-run -Werror"
  else
    echo "===[ clang-format ]==="
    echo "  skipped (no modified C++ files)"
  fi
fi

echo
if [[ ${#FAILED_CHECKS[@]} -gt 0 ]]; then
  echo "FAILED: ${#FAILED_CHECKS[@]} check(s) failed:"
  for c in "${FAILED_CHECKS[@]}"; do
    echo "  - ${c}"
  done
  exit 1
fi

echo "All presubmit checks passed."
