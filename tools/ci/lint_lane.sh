#!/usr/bin/env bash
#
# lint_lane.sh - the Lint workflow's checks, in the workflow's order, as one command.
#
# These checks read the whole tracked tree, git history or host clang-format 18, so they run here
# rather than as tests reachable from `bazel test //...`:
#
#   - tools/lint.sh: banned patterns, clang-format 18 and changed-method complexity
#   - tools/lint_braces_tests.py: brace enforcement through tools/lint.sh itself
#   - tools/check_design_doc_provenance.py (also //tools:design_doc_provenance_test)
#   - tools/check_private_details.py over every tracked file
#   - the lexical no-Rust-dependency verifier, which also scans paths .bazelignore hides
#
# Usage: tools/ci/lint_lane.sh
# Each check is a `::group::` in the Actions log and a row in the job summary; the first failing
# check ends the lane with its exit status.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
cd "${script_dir}/../.."

if [[ $# -ne 0 ]]; then
  sed -n '3,17p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2
  exit 2
fi

# shellcheck source=tools/ci/lane.sh
source "tools/ci/lane.sh"

lane_begin "Lint"
lane_phase "Check source lint and formatting" tools/lint.sh
lane_phase "Check brace lint regression" python3 tools/lint_braces_tests.py
lane_phase "Check design document provenance" python3 tools/check_design_doc_provenance.py
lane_phase "Check tracked files for private details" python3 tools/check_private_details.py
lane_phase "No-Rust-dependency verifier (blocking)" \
  python3 tools/rust_boundary/check_no_rust_dependencies.py --blocking default
