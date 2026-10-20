#!/usr/bin/env bash
#
# coverage_lane.sh - the Coverage workflow's measurement: tools/coverage.sh over the product tree.
#
# Usage: tools/ci/coverage_lane.sh [target...]
#   Targets default to $COVERAGE_TARGETS (whitespace-separated), which the workflow sets to the
#   complete product tree. Disk use before and after is logged in collapsible groups.
#
# tools/coverage.sh may legitimately write no report when every selected label is incompatible
# with the host and leaves coverage-report/coverage_skipped instead. Over the complete product
# tree that would mean a classification regression, and treating it as a skip would end the run
# green with no baseline, so the marker fails the lane.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
cd "${script_dir}/../.."

targets=("$@")
if [[ ${#targets[@]} -eq 0 ]]; then
  read -r -a targets <<<"${COVERAGE_TARGETS:-}"
fi
if [[ ${#targets[@]} -eq 0 ]]; then
  echo "coverage_lane.sh: no targets (pass them or set COVERAGE_TARGETS)" >&2
  exit 2
fi

disk() {
  echo "::group::Disk $1 coverage"
  df -h / "${GITHUB_WORKSPACE:-${PWD}}" "${RUNNER_TEMP:-${TMPDIR:-/tmp}}"
  echo "::endgroup::"
}

printf "Coverage targets:\n"
printf "  %s\n" "${targets[@]}"
disk before
tools/coverage.sh --quiet --no-html "${targets[@]}"
disk after

if [[ -f coverage-report/coverage_skipped ]]; then
  echo "::error::Coverage produced no report: every selected target was skipped as incompatible with this runner. The complete product tree must produce a report."
  exit 1
fi
