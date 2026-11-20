#!/usr/bin/env bash
#
# lint_lane.sh - run changed-file CI checks or a complete local lint audit.
#
# CI uses --changed BASE. With no arguments, the complete audit runs these checks:
#
#   - tools/lint.sh: banned patterns, the pinned clang-format over every tracked C++ file, and
#     changed-method complexity
#   - tools/lint_braces_tests.py: brace enforcement through tools/lint.sh itself
#   - tools/check_design_doc_provenance.py (also //tools:design_doc_provenance_test)
#   - tools/check_design_doc_index.py (also //tools:design_doc_index_test)
#   - tools/check_private_details.py over every tracked file
#   - tools/check_js_lint_coverage.py: every tracked JavaScript file is in the ESLint test
#   - the lexical no-Rust-dependency verifier, which also scans paths .bazelignore hides
#   - tools/ci/fuzz_lane_coverage.py: a Fuzz workflow phase soaks every fuzzer (a Bazel query)
#   - build_defs/check_local_gpu_tags.py: GPU test scheduling tags in every tracked BUILD file
#   - build_defs/check_variant_transitions.py: every raw transition() that writes a variant setting
#     is reviewed
#
# Usage: tools/ci/lint_lane.sh [--changed BASE]
# Each check is a `::group::` in the Actions log and a row in the job summary; the first failing
# check ends the lane with its exit status.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
cd "${script_dir}/../.."

if [[ $# -eq 2 && "$1" == "--changed" ]]; then
  source "tools/ci/lane.sh"
  lane_begin "Changed-file lint"
  exec python3 -m tools.ci.lint_changes run "$2"
fi

if [[ $# -ne 0 ]]; then
  sed -n '3,/^# check ends/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2
  exit 2
fi

# shellcheck source=tools/ci/lane.sh
source "tools/ci/lane.sh"

lane_begin "Lint"
lane_phase "Check source lint and formatting" tools/lint.sh
lane_phase "Check brace lint regression" python3 tools/lint_braces_tests.py
lane_phase "Check design document provenance" python3 tools/check_design_doc_provenance.py
lane_phase "Check design document index" python3 tools/check_design_doc_index.py
lane_phase "Check tracked files for private details" python3 tools/check_private_details.py
lane_phase "Check JavaScript lint coverage" python3 tools/check_js_lint_coverage.py
lane_phase "No-Rust-dependency verifier (blocking)" \
  python3 tools/rust_boundary/check_no_rust_dependencies.py --blocking default
lane_phase "Check every fuzzer is soaked by the Fuzz workflow" python3 tools/ci/fuzz_lane_coverage.py
lane_phase "Check GPU test scheduling tags" python3 build_defs/check_local_gpu_tags.py
lane_phase "Check variant transitions" python3 build_defs/check_variant_transitions.py
