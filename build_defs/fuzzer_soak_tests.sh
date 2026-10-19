#!/usr/bin/env bash
# Exercise the real soak launcher end to end: seed isolation, flag forwarding, artifact routing,
# signal exit propagation, timeout reports, and fuzzers that fail to start or exit at once. The
# fuzzer is a stub script or a system binary, so the checks stay hermetic and fast. Every check is
# on a status, output, or process state, never on elapsed time: a launcher that hangs fails at the
# Bazel test timeout instead.
set -uo pipefail

runner="${TEST_SRCDIR}/${TEST_WORKSPACE}/build_defs/fuzzer_soak_runner"
if [[ ! -x "${runner}" ]]; then
  echo "launcher not found at ${runner}" >&2
  exit 1
fi

failures=0
check() {
  if [[ "$2" != "$3" ]]; then
    echo "FAIL $1: expected [$3], got [$2]" >&2
    failures=$((failures + 1))
  fi
}

work="$(mktemp -d "${TEST_TMPDIR:-/tmp}/soak-tests.XXXXXX")"
trap 'rm -rf "${work}"' EXIT

# Seeds that share a basename must both reach the corpus, and must not be modified in place.
mkdir -p "${work}/a space" "${work}/b"
printf 'first' > "${work}/a space/seed"
printf 'second' > "${work}/b/seed"

cat > "${work}/stub" <<'STUB'
#!/usr/bin/env bash
corpus="${!#}"
for seed in "${corpus}"/*; do cat "${seed}"; echo; done | sort | tr '\n' ',' > "${corpus}/../listing"
echo "flags=$1 contents=$(cat "${corpus}/../listing")"
printf 'generated' > "${corpus}/new-finding"
for argument in "$@"; do
  case "${argument}" in
    -artifact_prefix=*) printf 'repro' > "${argument#-artifact_prefix=}crash" ;;
  esac
done
exit "${STUB_EXIT:-70}"
STUB
chmod +x "${work}/stub"

outputs="${work}/outputs"
mkdir -p "${outputs}"
observed="$(TEST_UNDECLARED_OUTPUTS_DIR="${outputs}" "${runner}" "${work}/stub" -runs=128 \
  "${work}/a space/seed" "${work}/b/seed")"
check "exit code is forwarded" "$?" "70"
check "flags are forwarded" "${observed%% contents=*}" "flags=-runs=128"
check "both same-name seeds are copied" "${observed##* contents=}" "first,second,"
check "first seed is untouched" "$(cat "${work}/a space/seed")" "first"
check "second seed is untouched" "$(cat "${work}/b/seed")" "second"
check "seed directory gains nothing" "$(ls "${work}/a space")" "seed"
check "findings are preserved" "$(cat "${outputs}/findings/crash")" "repro"

# An empty corpus is legal, and a fuzzer killed by a signal must surface as 128+N.
rm -rf "${outputs}"
mkdir -p "${outputs}"
cat > "${work}/killer" <<'KILL'
#!/usr/bin/env bash
corpus="${!#}"
if [[ -n "$(ls -A "${corpus}")" ]]; then
  echo "expected an empty corpus" >&2
  exit 1
fi
kill -ABRT $$
KILL
chmod +x "${work}/killer"
TEST_UNDECLARED_OUTPUTS_DIR="${outputs}" "${runner}" "${work}/killer" > /dev/null 2>&1
check "signal exit maps to 128+N" "$?" "134"

# A complete libFuzzer timeout report reaches its error line, keeps the fuzzer's own status, and
# reaches the launcher's stderr byte for byte.
cat > "${work}/timeout-report.expected" <<'LINES'
ALARM: working on the last Unit for 3 seconds
       and the timeout value is 2 (use -timeout=N to change)
==1== ERROR: libFuzzer: timeout after 3 seconds
SUMMARY: libFuzzer: timeout
LINES
printf 'output without a final newline' >> "${work}/timeout-report.expected"
cat > "${work}/timeout-report" <<'REPORT'
#!/usr/bin/env bash
cat "${0}.expected" >&2
exit 70
REPORT
chmod +x "${work}/timeout-report"
TEST_UNDECLARED_OUTPUTS_DIR="${outputs}" "${runner}" "${work}/timeout-report" \
  > /dev/null 2> "${work}/timeout-report.err"
check "reported timeout keeps its status" "$?" "70"
relayed="$(cmp -s "${work}/timeout-report.expected" "${work}/timeout-report.err" && echo same)"
check "reported timeout is relayed unchanged" "${relayed:-changed}" "same"

# libFuzzer writes its timeout report from a SIGALRM handler that allocates. When the alarm lands
# while the fuzz target holds the allocator lock, the handler deadlocks after its ALARM lines and
# the fuzzer never exits. The stub never exits on its own either, so only the launcher stopping it
# can end this step, and the launcher must then fail as a timeout.
cat > "${work}/stalled-report" <<'STALL'
#!/usr/bin/env bash
echo "$$" > "${0}.pid"
printf 'ALARM: working on the last Unit for 3 seconds\n' >&2
exec sleep 2147483647
STALL
chmod +x "${work}/stalled-report"
TEST_UNDECLARED_OUTPUTS_DIR="${outputs}" "${runner}" "${work}/stalled-report" \
  > /dev/null 2> "${work}/stalled-report.err"
check "stalled timeout report fails as a timeout" "$?" "70"
check "stalled fuzzer is stopped" \
  "$(kill -0 "$(cat "${work}/stalled-report.pid")" 2> /dev/null && echo running || echo stopped)" \
  "stopped"
check "stalled timeout report is explained" \
  "$(grep -c 'did not finish its timeout report' "${work}/stalled-report.err")" "1"
check "complete timeout report is not treated as stalled" \
  "$(grep -c 'did not finish its timeout report' "${work}/timeout-report.err")" "0"

# The per-input timeout is only a hang guard, so a slow unit must fail on its own. libFuzzer reports
# a unit slower than -report_slow_units with a "Slowest unit" line, saves it and names the file,
# keeps fuzzing, and exits zero. For every report after INITED, which marks the start of mutation,
# the launcher replays that unit: a slow, failing or untimed replay fails the soak, and a fast one
# means the process stalled. Before INITED, libFuzzer is replaying the seed corpus, the first
# inputs of a cold process, and those keep the hang guard's headroom. The stub reports one slow
# unit per word of SEED_REPORTS and MUTATION_REPORTS. Each word is what replaying that unit does:
# a time in ms, "none" for no time, "hang" for a timeout report that never finishes, "unsaved"
# for a report without a saved file, or "outside" for a file named outside the findings prefix.
# STUB_PARTIAL_LINE puts each report after target output that has no final newline.
cat > "${work}/slow-unit" <<'SLOW'
#!/usr/bin/env bash
for argument in "$@"; do
  case "${argument}" in
    -artifact_prefix=*) prefix="${argument#-artifact_prefix=}" ;;
    -runs=1)
      echo "replayed with $*" >> "${SLOW_REPLAYS}"
      replay="$(cat "${!#}")"
      if [[ "${replay}" == hang ]]; then
        printf 'ALARM: working on the last Unit for 61 seconds\n' >&2
        exec sleep 2147483647
      fi
      if [[ "${replay}" != none ]]; then
        echo "Executed ${!#} in ${replay} ms" >&2
      fi
      exit "${STUB_REPLAY_EXIT:-0}"
      ;;
  esac
done
saved=0
report() {
  [[ -n "${STUB_PARTIAL_LINE:-}" ]] && printf 'target output without a newline' >&2
  echo 'Slowest unit: 12 s:' >&2
  if [[ "$1" == outside ]]; then
    printf '5' > "${prefix}../slow-unit-outside"
    echo "artifact_prefix='${prefix}'; Test unit written to ${prefix}../slow-unit-outside" >&2
  elif [[ "$1" != unsaved ]]; then
    saved=$((saved + 1))
    printf '%s' "$1" > "${prefix}slow-unit-${saved}"
    echo "artifact_prefix='${prefix}'; Test unit written to ${prefix}slow-unit-${saved}" >&2
  fi
}
for replay in ${SEED_REPORTS}; do report "${replay}"; done
printf '#2\tINITED cov: 1 ft: 1 corp: 1/1b exec/s: 0 rss: 1Mb\n' >&2
for replay in ${MUTATION_REPORTS}; do report "${replay}"; done
echo 'Done 3 runs in 1 second(s)' >&2
exit "${STUB_EXIT:-0}"
SLOW
chmod +x "${work}/slow-unit"
# Arguments: seed-phase reports, mutation-phase reports. STUB_EXIT and STUB_REPLAY_EXIT may be set
# by the caller. Prints the status, the launcher's slow, stall and stalled-report message counts,
# the number of replays and the first replay's arguments, and copies the launcher's stderr to the
# test log.
slow_unit() {
  rm -rf "${outputs}" "${work}/replays"
  mkdir -p "${outputs}"
  touch "${work}/replays"
  SEED_REPORTS="$1" MUTATION_REPORTS="$2" SLOW_REPLAYS="${work}/replays" \
    TEST_UNDECLARED_OUTPUTS_DIR="${outputs}" "${runner}" "${work}/slow-unit" -timeout=60 \
    -report_slow_units=10 > /dev/null 2> "${work}/slow-unit.err"
  local status=$?
  local err="${work}/slow-unit.err"
  echo "status=${status}" \
    "slow=$(grep -c 'reported a slow unit after the seed corpus replay' "${err}")" \
    "stall=$(grep -c 'so the process stalled' "${err}")" \
    "stalled_report=$(grep -c 'did not finish its timeout report' "${err}")" \
    "replays=$(grep -c . "${work}/replays") first=[$(head -n 1 "${work}/replays")]"
  # The launcher's messages, so a failed check can be read from the test log.
  sed "s/^/slow unit [$1|$2]: /" "${err}" >&2
}
findings="${outputs}/findings"
first="first=[replayed with -timeout=60 -artifact_prefix=${findings}/replay- -runs=1"
first+=" ${findings}/slow-unit-1]"
check "slow unit that replays slowly fails as a timeout" "$(slow_unit '' 12000)" \
  "status=70 slow=1 stall=0 stalled_report=0 replays=1 ${first}"
check "slow unit that replays fast is a stall and passes" "$(slow_unit '' 5)" \
  "status=0 slow=0 stall=1 stalled_report=0 replays=1 ${first}"
check "a later stall does not hide an earlier slow unit" "$(slow_unit '' '12000 5')" \
  "status=70 slow=1 stall=0 stalled_report=0 replays=1 ${first}"
check "every slow unit is replayed" "$(slow_unit '' '5 12000')" \
  "status=70 slow=1 stall=1 stalled_report=0 replays=2 ${first}"
check "slow unit whose replay prints no time fails as a timeout" "$(slow_unit '' none)" \
  "status=70 slow=1 stall=0 stalled_report=0 replays=1 ${first}"
check "slow unit without a saved file fails as a timeout" "$(slow_unit '' unsaved)" \
  "status=70 slow=1 stall=0 stalled_report=0 replays=0 first=[]"
check "slow unit named outside the findings directory fails" "$(slow_unit '' outside)" \
  "status=70 slow=1 stall=0 stalled_report=0 replays=0 first=[]"
check "slow unit after a partial target line is still replayed" \
  "$(STUB_PARTIAL_LINE=1 slow_unit '' 12000)" \
  "status=70 slow=1 stall=0 stalled_report=0 replays=1 ${first}"
check "replay that stalls in its timeout report is stopped" "$(slow_unit '' hang)" \
  "status=70 slow=1 stall=0 stalled_report=1 replays=1 ${first}"
check "failing replay keeps its status" "$(STUB_REPLAY_EXIT=3 slow_unit '' 5)" \
  "status=3 slow=1 stall=0 stalled_report=0 replays=1 ${first}"
check "slow seed replay keeps the hang guard headroom" "$(slow_unit 12000 '')" \
  "status=0 slow=0 stall=0 stalled_report=0 replays=0 first=[]"
check "fuzzer failure after a slow unit keeps its status" "$(STUB_EXIT=1 slow_unit '' 12000)" \
  "status=1 slow=0 stall=0 stalled_report=0 replays=0 first=[]"

# A fuzzer that fails to start, or exits at once, finishes while the launcher is still connecting
# its stderr relay. Bash 3.2 does not retry an open() that SIGCHLD interrupts, and on macOS a FIFO
# reader can miss end of file when its writer exits that quickly, so a relay built on FIFOs can
# hang here. Each attempt must return the fuzzer's status and relay its message; a hung attempt
# holds the test until the Bazel timeout, and the last progress line names it.
# Prints the expected result, or the first different one among the attempts.
quick_exits() {
  local fuzzer="$1" expected="$2" attempts="$3" attempt result
  for ((attempt = 0; attempt < attempts; attempt++)); do
    echo "quick exit: ${fuzzer##*/} attempt ${attempt}" >&2
    TEST_UNDECLARED_OUTPUTS_DIR="${outputs}" "${runner}" "${fuzzer}" \
      > /dev/null 2> "${work}/quick-exit.err"
    result="status=$? message=$(grep -c 'No such file' "${work}/quick-exit.err")"
    if [[ "${result}" != "${expected}" ]]; then
      echo "${result}"
      return
    fi
  done
  echo "${expected}"
}
check "fuzzer that cannot start returns its status" \
  "$(quick_exits "${work}/missing-fuzzer" "status=127 message=1" 25)" "status=127 message=1"
check "fuzzer that exits at once returns its status" \
  "$(quick_exits "$(type -P false)" "status=1 message=0" 25)" "status=1 message=0"

# A seed that cannot be staged must fail the run. The stub exits zero, so a launcher that ignored
# the copy failure would report success with a silently shortened corpus.
cat > "${work}/ok" <<'OK'
#!/usr/bin/env bash
exit 0
OK
chmod +x "${work}/ok"
TEST_UNDECLARED_OUTPUTS_DIR="${outputs}" "${runner}" "${work}/ok" "${work}/missing-seed" \
  > /dev/null 2>&1
check "unstageable seed fails the run" "$(($? != 0))" "1"

exit "${failures}"
