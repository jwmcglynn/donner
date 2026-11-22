#!/usr/bin/env bash
# Runs the gtest_timeout_main hang fixture in subprocesses and checks the watchdog message each one
# reports: which limit fired, where, and when the alarm was armed and for how long.
#
# Usage: gtest_timeout_main_watchdog_tests.sh <fixture binary> simulated|inherited
#   simulated  Scenarios under their own TEST_TIMEOUT values, run in parallel. Each verdict comes
#              from the child's exit status and message, never from wall-clock timing here; a
#              generous kill guard only stops a child the watchdog failed to stop.
#   inherited  One stalled case under this target's own TEST_TIMEOUT, with Bazel as the killer.
#              Used by the manual small, medium and large targets.

set -uo pipefail

readonly fixture="$1"
readonly mode="$2"
readonly out_dir="${TEST_TMPDIR}/watchdog_scenarios"
readonly guard_seconds=45
# TEST_TIMEOUT for the simulated deadline scenarios, and the deadline it yields after the 5 s
# margin. Eleven seconds leaves ample time for process start-up under sanitizers and loaded hosts.
readonly simulated_timeout=16
readonly simulated_deadline_ms=11000
readonly deadline_prefix="[donner] test reached the watchdog deadline ahead of Bazel's TEST_TIMEOUT: "
readonly per_case_prefix="[donner] test exceeded per-case timeout: "
failures=0
mkdir -p "${out_dir}"

# start_scenario <name> [VAR=value ...] -- [fixture args ...]
# Starts the fixture in the background with TEST_TIMEOUT cleared unless the assignments set it (in
# inherited mode the caller's TEST_TIMEOUT is kept). The fixture must not touch this target's own
# result files or sharding protocol: its deliberate exit 124 would otherwise leave Bazel's
# premature-exit marker or replace this target's XML. Writes <name>.out and <name>.status.
start_scenario() {
  local name="$1"
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

  (
    env "${clear[@]}" ${assignments[@]+"${assignments[@]}"} "${fixture}" "$@" \
      >"${out_dir}/${name}.out" 2>&1 &
    child=$!
    killer=""
    if [[ "${mode}" != inherited ]]; then
      (
        exec >/dev/null 2>&1
        sleep "${guard_seconds}" &
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
    echo "${status}" >"${out_dir}/${name}.status"
  ) &
}

fail() {
  local name="$1"
  local reason="$2"
  failures=$((failures + 1))
  echo "FAIL: ${name}: ${reason}"
  echo "  exit status $(cat "${out_dir}/${name}.status"); fixture output follows"
  sed 's/^/  | /' "${out_dir}/${name}.out"
}

# watchdog_line <name> <message prefix and location>
# Prints the watchdog message for that location, or nothing.
watchdog_line() {
  grep -F -- "$2 (alarm armed " "${out_dir}/$1.out" | head -n 1
}

# alarm_field <line> <1|2|3>: the armed ms, interval ms or deadline ms of a deadline message.
alarm_field() {
  sed -E "s/.*alarm armed ([0-9]+) ms after main\(\) for ([0-9]+) ms; deadline ([0-9]+) ms.*/\\$2/" \
    <<<"$1"
}

# expect_deadline <name> <location> <deadline ms> [minimum armed ms]
expect_deadline() {
  local name="$1" where="$2" deadline_ms="$3" min_armed_ms="${4:-0}"
  local status line armed interval deadline
  status="$(cat "${out_dir}/${name}.status")"
  line="$(watchdog_line "${name}" "${deadline_prefix}${where}")"
  if [[ "${status}" -ne 124 || -z "${line}" ]]; then
    fail "${name}" "expected exit 124 and: ${deadline_prefix}${where} (alarm armed ..."
    return
  fi
  armed="$(alarm_field "${line}" 1)"
  interval="$(alarm_field "${line}" 2)"
  deadline="$(alarm_field "${line}" 3)"
  if [[ ! "${armed}${interval}${deadline}" =~ ^[0-9]+$ ]]; then
    fail "${name}" "could not parse the alarm timing in: ${line}"
  elif [[ "${deadline}" -ne "${deadline_ms}" ]]; then
    fail "${name}" "deadline ${deadline} ms, expected ${deadline_ms} ms"
  elif [[ $((armed + interval)) -ne "${deadline}" ]]; then
    fail "${name}" "alarm armed at ${armed} ms for ${interval} ms does not end at the deadline"
  elif [[ "${armed}" -lt "${min_armed_ms}" ]]; then
    fail "${name}" "alarm armed at ${armed} ms, expected at least ${min_armed_ms} ms"
  else
    echo "PASS: ${name}"
    echo "  | ${line}"
  fi
}

# expect_per_case <name> <location> <budget ms>
expect_per_case() {
  local name="$1" where="$2" budget_ms="$3"
  local status line interval
  status="$(cat "${out_dir}/${name}.status")"
  line="$(watchdog_line "${name}" "${per_case_prefix}${where}")"
  if [[ "${status}" -ne 124 || -z "${line}" ]]; then
    fail "${name}" "expected exit 124 and: ${per_case_prefix}${where} (alarm armed ..."
    return
  fi
  interval="$(sed -E 's/.*alarm armed [0-9]+ ms after main\(\) for ([0-9]+) ms\).*/\1/' <<<"${line}")"
  if [[ "${interval}" != "${budget_ms}" ]]; then
    fail "${name}" "alarm interval ${interval} ms, expected the ${budget_ms} ms budget"
  else
    echo "PASS: ${name}"
    echo "  | ${line}"
  fi
}

echo "TEST_TIMEOUT for this target: ${TEST_TIMEOUT:-unset}"

if [[ "${mode}" == inherited ]]; then
  if [[ "${TEST_TIMEOUT}" -lt 10 ]]; then
    expected_deadline_ms=$((TEST_TIMEOUT * 500))
  else
    expected_deadline_ms=$(((TEST_TIMEOUT - 5) * 1000))
  fi
  # A per-case budget longer than any test size leaves the deadline derived from TEST_TIMEOUT as
  # the only limit, so this checks that deadline against Bazel's real timeout for this size.
  start_scenario inherited_stall -- --gtest_filter=HangFixture.Hangs \
    --donner_test_timeout_seconds=86400
  wait
  expect_deadline inherited_stall HangFixture.Hangs "${expected_deadline_ms}"
  exit $((failures > 0))
fi

readonly T="TEST_TIMEOUT=${simulated_timeout}"
start_scenario first_case "${T}" -- --gtest_filter=HangFixture.Hangs
start_scenario long_override "${T}" -- --gtest_filter=HangFixture.Hangs \
  --donner_test_timeout_seconds=100
# Time spent in an earlier case counts against the deadline: the stalled case's alarm is armed
# after it and still ends at the same deadline.
start_scenario after_slow_case "${T}" DONNER_HANG_FIXTURE_SPEND_MS=500 -- \
  --gtest_filter=HangFixture.SpendsTime:HangFixture.Hangs --donner_test_timeout_seconds=100
start_scenario suite_setup "${T}" DONNER_HANG_FIXTURE_HANG_IN_SUITE_SETUP=1 -- \
  '--gtest_filter=HangInSuiteSetup.*'
start_scenario suite_teardown "${T}" DONNER_HANG_FIXTURE_HANG_IN_SUITE_TEARDOWN=1 -- \
  '--gtest_filter=HangInSuiteTeardown.*'
start_scenario short_budget TEST_TIMEOUT=600 -- --gtest_filter=HangFixture.Hangs \
  --donner_test_timeout_seconds=1
start_scenario env_budget DONNER_TEST_TIMEOUT_SECONDS=1 -- --gtest_filter=HangFixture.Hangs
start_scenario malformed_timeout TEST_TIMEOUT=abc -- --gtest_filter=HangFixture.Hangs \
  --donner_test_timeout_seconds=1
start_scenario passing TEST_TIMEOUT=600 -- --gtest_filter=HangFixture.Passes
wait

expect_deadline first_case HangFixture.Hangs "${simulated_deadline_ms}"
expect_deadline long_override HangFixture.Hangs "${simulated_deadline_ms}"
expect_deadline after_slow_case HangFixture.Hangs "${simulated_deadline_ms}" 500
expect_deadline suite_setup "outside any test case, in setup of test suite HangInSuiteSetup" \
  "${simulated_deadline_ms}"
expect_deadline suite_teardown "outside any test case, after HangInSuiteTeardown.Runs" \
  "${simulated_deadline_ms}"
expect_per_case short_budget HangFixture.Hangs 1000
expect_per_case env_budget HangFixture.Hangs 1000
expect_per_case malformed_timeout HangFixture.Hangs 1000
if [[ "$(cat "${out_dir}/passing.status")" -eq 0 ]] &&
  grep -qF "[  PASSED  ] 1 test." "${out_dir}/passing.out"; then
  echo "PASS: passing"
else
  fail passing "expected exit 0 and one passing case"
fi

exit $((failures > 0))
