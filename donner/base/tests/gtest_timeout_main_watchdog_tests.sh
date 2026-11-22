#!/usr/bin/env bash
# Runs the gtest_timeout_main hang fixture in a subprocess and checks that the watchdog names the
# stalled case before the Bazel test timeout ends the process.
#
# Usage: gtest_timeout_main_watchdog_tests.sh <fixture binary> simulated|inherited
#   simulated  Fast scenarios against short timeouts. This script plays Bazel's part: it sets
#              TEST_TIMEOUT for the child and SIGKILLs the child when that time runs out, so a
#              watchdog that fires too late shows up as status 137 instead of 124.
#   inherited  One stalled case under this target's own TEST_TIMEOUT, with Bazel as the killer.
#              Used by the manual small, medium and large targets.

set -uo pipefail

readonly fixture="$1"
readonly mode="$2"
readonly output_file="${TEST_TMPDIR}/fixture_output.txt"
readonly deadline_message="ahead of Bazel's TEST_TIMEOUT: "
readonly per_case_message="exceeded per-case timeout: "
failures=0
status=0

# run_child <kill after seconds|none> [VAR=value ...] -- [fixture args ...]
# Runs the fixture with TEST_TIMEOUT cleared unless the assignments set it (in inherited mode the
# caller's TEST_TIMEOUT is kept). The fixture must not touch this target's own result files or
# sharding protocol: its deliberate exit 124 would otherwise leave Bazel's premature-exit marker or
# replace this target's XML. Sets `status` to the fixture's exit status.
run_child() {
  local kill_after="$1"
  shift
  local -a assignments=()
  while [[ "$1" != "--" ]]; do
    assignments+=("$1")
    shift
  done
  shift

  local -a clear=(-u XML_OUTPUT_FILE -u TEST_PREMATURE_EXIT_FILE -u TEST_TOTAL_SHARDS
    -u TEST_SHARD_INDEX -u TEST_SHARD_STATUS_FILE)
  if [[ "${mode}" != inherited ]]; then
    clear+=(-u TEST_TIMEOUT)
  fi
  env "${clear[@]}" ${assignments[@]+"${assignments[@]}"} "${fixture}" "$@" \
    >"${output_file}" 2>&1 &
  local child=$!

  local killer=""
  if [[ "${kill_after}" != none ]]; then
    (
      exec >/dev/null 2>&1
      sleep "${kill_after}" &
      sleeper=$!
      trap 'kill "${sleeper}"; exit 0' TERM
      wait "${sleeper}"
      kill -KILL "${child}"
    ) &
    killer=$!
  fi

  wait "${child}"
  status=$?
  if [[ -n "${killer}" ]]; then
    kill "${killer}" 2>/dev/null
    wait "${killer}" 2>/dev/null
  fi
}

# expect <scenario> <expected status> <expected output substring>
expect() {
  local scenario="$1"
  local expected_status="$2"
  local expected_output="$3"
  if [[ "${status}" -eq "${expected_status}" ]] &&
    grep -qF -- "${expected_output}" "${output_file}"; then
    echo "PASS: ${scenario}"
    return
  fi
  failures=$((failures + 1))
  echo "FAIL: ${scenario}"
  echo "  expected status ${expected_status} and output containing: ${expected_output}"
  echo "  actual status ${status}; fixture output follows"
  sed 's/^/  | /' "${output_file}"
}

echo "TEST_TIMEOUT for this target: ${TEST_TIMEOUT:-unset}"

if [[ "${mode}" == inherited ]]; then
  # A per-case budget longer than any test size leaves the deadline derived from TEST_TIMEOUT as
  # the only limit, so this checks that deadline against Bazel's real timeout for this size.
  run_child none -- --gtest_filter=HangFixture.Hangs --donner_test_timeout_seconds=86400
  expect "stall under the target's own TEST_TIMEOUT" 124 \
    "${deadline_message}HangFixture.Hangs"
  exit $((failures > 0))
fi

run_child 3 TEST_TIMEOUT=3 -- --gtest_filter=HangFixture.Hangs
expect "stall in the first case" 124 "${deadline_message}HangFixture.Hangs"

run_child 3 TEST_TIMEOUT=3 -- --gtest_filter=HangFixture.Hangs --donner_test_timeout_seconds=100
expect "explicit per-case budget longer than TEST_TIMEOUT" 124 \
  "${deadline_message}HangFixture.Hangs"

# TEST_TIMEOUT=12 leaves a 5 s margin, so the deadline is 7 s. The 6 s spent in the first case must
# count against it, or the stalled second case would get a fresh 7 s and outlive the timeout.
run_child 12 TEST_TIMEOUT=12 DONNER_HANG_FIXTURE_SPEND_MS=6000 -- \
  --gtest_filter=HangFixture.SpendsTime:HangFixture.Hangs --donner_test_timeout_seconds=100
expect "stall after time spent in an earlier case" 124 "${deadline_message}HangFixture.Hangs"

run_child 3 TEST_TIMEOUT=3 DONNER_HANG_FIXTURE_HANG_IN_SUITE_SETUP=1 -- \
  '--gtest_filter=HangInSuiteSetup.*'
expect "stall outside any test case" 124 \
  "${deadline_message}outside any test case, in setup of test suite HangInSuiteSetup"

run_child 30 TEST_TIMEOUT=30 -- --gtest_filter=HangFixture.Hangs --donner_test_timeout_seconds=1
expect "per-case budget shorter than the deadline" 124 "${per_case_message}HangFixture.Hangs"

run_child 20 DONNER_TEST_TIMEOUT_SECONDS=1 -- --gtest_filter=HangFixture.Hangs
expect "no TEST_TIMEOUT, environment per-case budget" 124 "${per_case_message}HangFixture.Hangs"

run_child 20 TEST_TIMEOUT=abc -- --gtest_filter=HangFixture.Hangs --donner_test_timeout_seconds=1
expect "malformed TEST_TIMEOUT keeps the per-case budget" 124 \
  "${per_case_message}HangFixture.Hangs"

run_child 3 TEST_TIMEOUT=3 -- --gtest_filter=HangFixture.Passes
expect "passing case" 0 "[  PASSED  ] 1 test."

exit $((failures > 0))
