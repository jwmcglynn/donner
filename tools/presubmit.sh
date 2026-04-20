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
#   tools/presubmit.sh --variants      # Run key Bazel variant checks
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
RUN_VARIANTS=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --fast) FAST=1; RUN_BAZEL=0; shift ;;
    --variants) RUN_VARIANTS=1; shift ;;
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
declare -a VARIANT_RESULTS=()
declare -a VARIANT_WARNINGS=()

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

run_variant_check() {
  local config="$1"
  local name="bazel test --config=dev --config=${config} //..."
  local skip_reason
  if skip_reason="$(get_variant_skip_reason "${config}")"; then
    echo "===[ ${name} ]==="
    echo "  skipped (${skip_reason})"
    VARIANT_RESULTS+=("${config}: SKIP")
    VARIANT_WARNINGS+=("${skip_reason}")
    return 0
  fi

  echo "===[ ${name} ]==="
  if bazel test --config=dev --config="${config}" //...; then
    echo "  ok"
    VARIANT_RESULTS+=("${config}: PASS")
  else
    echo "  FAILED"
    FAILED_CHECKS+=("${name}")
    VARIANT_RESULTS+=("${config}: FAIL")
  fi
  return 0  # Always continue
}

get_variant_skip_reason() {
  local config="$1"
  if [[ "${config}" == "tiny" ]]; then
    echo "The tiny tier disables filters/text, and clean-main //... tests are not yet config-aware enough for this lane"
    return 0
  fi

  if [[ "${config}" != "geode" ]]; then
    return 1
  fi

  local drm_device
  for drm_device in /sys/class/drm/card*/device; do
    [[ -d "${drm_device}" ]] || continue

    local vendor_id=""
    local driver_name=""
    if [[ -r "${drm_device}/vendor" ]]; then
      vendor_id="$(<"${drm_device}/vendor")"
    fi
    if [[ -r "${drm_device}/uevent" ]]; then
      driver_name="$(grep '^DRIVER=' "${drm_device}/uevent" | cut -d= -f2 || true)"
    fi

    if [[ "${vendor_id}" == "0x8086" && "${driver_name}" == "xe" ]]; then
      echo "Intel Arc Xe hosts currently fail geode tests on clean main; skipping this variant"
      return 0
    fi
  done

  return 1
}

# 1. CMake generation validation (generator bugs + output integrity).
#    Runs outside bazel because gen_cmakelists.py uses bazel query which
#    can't be reentrantly invoked from inside bazel test. See PR #3 / the
#    aspect design doc for the plan to bring this into bazel natively.
if [[ "${RUN_CMAKE}" == "1" ]]; then
  run_check "cmake validation" python3 tools/cmake/gen_cmakelists.py --check
fi

if [[ "${RUN_VARIANTS}" == "1" ]]; then
  # 2. Variant matrix smoke tests for select()- and compatibility-sensitive
  #    changes. `--config=dev` disables backend test transitions so each
  #    variant stays scoped to the selected matrix entry. Keep these opt-in so
  #    the default presubmit stays unchanged. Tiny is currently skipped until
  #    config-aware test filtering exists for the no-filters/no-text tier.
  run_variant_check "tiny"
  run_variant_check "text-full"
  run_variant_check "geode"
elif [[ "${RUN_BAZEL}" == "1" ]]; then
  # 2. `bazel test //...` — runs all unit tests AND all auto-generated lint
  #    tests (banned_patterns per-library). A new `long long` or UDL operator
  #    in a source file now fails here automatically.
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
if [[ ${#VARIANT_RESULTS[@]} -gt 0 ]]; then
  echo "Variant summary:"
  for result in "${VARIANT_RESULTS[@]}"; do
    echo "  - ${result}"
  done
  echo
fi

if [[ ${#VARIANT_WARNINGS[@]} -gt 0 ]]; then
  echo "Variant warnings:"
  for warning in "${VARIANT_WARNINGS[@]}"; do
    echo "  - ${warning}"
  done
  echo
fi

if [[ ${#FAILED_CHECKS[@]} -gt 0 ]]; then
  echo "FAILED: ${#FAILED_CHECKS[@]} check(s) failed:"
  for c in "${FAILED_CHECKS[@]}"; do
    echo "  - ${c}"
  done
  exit 1
fi

echo "All presubmit checks passed."
