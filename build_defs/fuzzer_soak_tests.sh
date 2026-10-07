#!/usr/bin/env bash
# Exercise the real soak launcher end to end: seed isolation, flag forwarding, artifact routing,
# signal exit propagation, timeout reports, and fuzzers that fail to start or exit at once. The
# fuzzer is a stub script or a system binary, so the checks stay hermetic and fast.
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
start="${SECONDS}"
TEST_UNDECLARED_OUTPUTS_DIR="${outputs}" "${runner}" "${work}/timeout-report" \
  > /dev/null 2> "${work}/timeout-report.err"
check "reported timeout keeps its status" "$?" "70"
check "reported timeout returns at once" "$((SECONDS - start < 4))" "1"
relayed="$(cmp -s "${work}/timeout-report.expected" "${work}/timeout-report.err" && echo same)"
check "reported timeout is relayed unchanged" "${relayed:-changed}" "same"

# libFuzzer writes its timeout report from a SIGALRM handler that allocates. When the alarm lands
# while the fuzz target holds the allocator lock, the handler deadlocks after its ALARM lines and
# the fuzzer never exits. The launcher must stop it and fail as a timeout well before the stub's
# 30 s sleep ends; a launcher that waits reports the sleep's success instead.
cat > "${work}/stalled-report" <<'STALL'
#!/usr/bin/env bash
echo "$$" > "${0}.pid"
printf 'ALARM: working on the last Unit for 3 seconds\n' >&2
exec sleep 30
STALL
chmod +x "${work}/stalled-report"
start="${SECONDS}"
TEST_UNDECLARED_OUTPUTS_DIR="${outputs}" "${runner}" "${work}/stalled-report" \
  > /dev/null 2> "${work}/stalled-report.err"
check "stalled timeout report fails as a timeout" "$?" "70"
check "stalled timeout report fails promptly" "$((SECONDS - start < 20))" "1"
check "stalled fuzzer is stopped" \
  "$(kill -0 "$(cat "${work}/stalled-report.pid")" 2> /dev/null && echo running || echo stopped)" \
  "stopped"
check "stalled timeout report is explained" \
  "$(grep -c 'did not finish its timeout report' "${work}/stalled-report.err")" "1"

# A fuzzer that fails to start, or exits at once, finishes while the launcher is still connecting
# its stderr relay. Bash 3.2 does not retry an open() that SIGCHLD interrupts, and on macOS a FIFO
# reader can miss end of file when its writer exits that quickly, so a relay built on FIFOs can
# hang here. Each attempt must promptly return the fuzzer's status and relay its message.
kill_tree() {
  local child
  for child in $(pgrep -P "$1"); do
    kill_tree "${child}"
  done
  kill -KILL "$1" 2> /dev/null
}
# Prints the expected result, or the first different one among the attempts; an attempt still
# running after 10 s is stopped.
quick_exits() {
  local fuzzer="$1" expected="$2" attempts="$3" attempt launcher result
  for ((attempt = 0; attempt < attempts; attempt++)); do
    launcher=""
    result=""
    {
      IFS= read -r launcher
      IFS= read -r -t 10 result
    } < <(
      TEST_UNDECLARED_OUTPUTS_DIR="${outputs}" "${runner}" "${fuzzer}" \
        > /dev/null 2> "${work}/quick-exit.err" &
      echo "$!"
      wait "$!"
      echo "status=$? message=$(grep -c 'No such file' "${work}/quick-exit.err")"
    )
    if [[ -z "${result}" && -n "${launcher}" ]]; then
      kill_tree "${launcher}"
      result="hung on attempt ${attempt}"
    fi
    if [[ "${result}" != "${expected}" ]]; then
      echo "${result}"
      return
    fi
  done
  echo "${expected}"
}
check "fuzzer that cannot start returns promptly" \
  "$(quick_exits "${work}/missing-fuzzer" "status=127 message=1" 25)" "status=127 message=1"
check "fuzzer that exits at once returns promptly" \
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
