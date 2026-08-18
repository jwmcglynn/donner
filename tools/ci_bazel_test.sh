#!/usr/bin/env bash
#
# ci_bazel_test.sh - run a bazel test command, tolerating an empty test set.
#
# Change-based CI lanes build and test only the targets a diff impacts. That
# selection can legitimately contain zero TEST targets: a docs filegroup, a
# tool-only change, or a diff confined to a vendored subtree whose own tests are
# run from that subtree's workspace rather than from this one. `bazel test`
# treats an empty test set as an error and exits 4 with "No test targets were
# found, yet testing was requested", so a change with nothing to test turns the
# lane red with a failure no rerun can clear and no edit to the change can fix.
#
# Bazel reserves exit code 4 for exactly that condition, and reports it only
# after the requested targets were built successfully. Mapping it to success
# therefore cannot hide a build break (exit 1), a failing or flaky test
# (exit 3), a bad command line (exit 2), or an internal/environment error (the
# 3x codes): every other status is passed through unchanged, so the step still
# fails on all of them.
#
# An empty selection produced by FAILING selection tooling cannot reach this
# wrapper: the job that derives the target set aborts on any tooling error, so
# its lanes never run at all, and a selection that legitimately comes back
# empty is widened to the full target set first. Callers reject an empty
# pattern list themselves before invoking this script.
#
# What this wrapper cannot see is selection tooling that SUCCEEDS while being
# wrong: an under-inclusive set that omits tests the change should have run
# looks exactly like a set that genuinely has none. That residual belongs to
# the derivation step and its own tests, not here.
#
# Usage:
#   tools/ci_bazel_test.sh bazel test --config=ci //pkg:target ...
#   tools/ci_bazel_test.sh <wrapper> <wrapper args...> bazel test ...
#
# The second form exists because some lanes run bazel under a logging wrapper.
# Any intermediate wrapper must propagate bazel's exit status verbatim.

set -uo pipefail

# Bazel's NO_TESTS_FOUND: "Build successful but no tests were found even though
# testing was requested."
readonly kNoTestsFoundExitCode=4

if [[ $# -eq 0 ]]; then
  echo "ci_bazel_test.sh: expected a command to run" >&2
  exit 2
fi

"$@"
status=$?

if [[ "${status}" -eq "${kNoTestsFoundExitCode}" ]]; then
  echo "::notice::No test targets in the selected target set; the selection built successfully and there was nothing to test."
  echo "ci_bazel_test.sh: tolerating exit ${kNoTestsFoundExitCode} (no test targets) from: $*"
  exit 0
fi

exit "${status}"
