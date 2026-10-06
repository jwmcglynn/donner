#!/usr/bin/env bash
# Run libFuzzer against a writable copy of the declared seed corpus.
#
# Seeds arrive as read-only runfiles and may share a basename, so they are copied into a scratch
# directory under numeric names before the fuzzer runs. Keeping this launcher in shell also keeps
# an interpreter out of the soak target's runfiles: a Python launcher resolves its runtime library
# through an rpath relative to its own location, which does not survive execution from a relocated
# content-addressed runfiles tree.
#
# libFuzzer reports a per-input timeout from its SIGALRM handler, and that report allocates (the
# artifact path, the artifact file's stdio buffer). When the alarm interrupts the fuzz target while
# it holds the allocator lock, the handler deadlocks after printing its ALARM lines and the fuzzer
# runs until the test deadline with no timeout report. The launcher therefore relays the fuzzer's
# stderr unchanged, and when a timeout report does not reach libFuzzer's error line within a grace
# period, it kills the fuzzer and fails with libFuzzer's timeout status.
set -euo pipefail

binary="$1"
shift

scratch="$(mktemp -d "${TEST_TMPDIR:-${TMPDIR:-/tmp}}/fuzzer-soak.XXXXXX")"
trap 'rm -rf "${scratch}"' EXIT
corpus="${scratch}/corpus"
mkdir -p "${corpus}"

flags=()
index=0
for argument in "$@"; do
  case "${argument}" in
    -*) flags+=("${argument}") ;;
    *)
      cp -- "${argument}" "${corpus}/${index}"
      index=$((index + 1))
      ;;
  esac
done

artifacts="${TEST_UNDECLARED_OUTPUTS_DIR:-${scratch}}/findings"
mkdir -p "${artifacts}"

readonly alarm_line='ALARM: working on the last Unit for '
readonly error_line='ERROR: libFuzzer: timeout after '
readonly end_of_output='fuzzer-soak: end of fuzzer output'
# A healthy report prints its error line milliseconds after the ALARM lines.
readonly report_grace_seconds=5
# libFuzzer's default -timeout_exitcode.
readonly timeout_status=70

# Staging above runs under errexit, so a seed that cannot be copied fails the run rather than
# silently shrinking the corpus. The fuzzer's status is the launcher's, including 128+N when a
# signal kills it; only a stalled timeout report replaces it.
set +e
# The relay uses only anonymous pipes: on macOS a FIFO reader can miss end of file after its last
# writer exits, and bash 3.2 does not retry a FIFO open() that SIGCHLD interrupts. tee copies the
# fuzzer's stderr to this launcher's stderr and to grep, whose matches and final end marker arrive
# on fd 3. The fuzzer runs inside that process substitution, so its pid and status come back
# through files; the status is written before the end marker can arrive.
exec 5>&1 6>&2
exec 3< <(
  {
    "${binary}" ${flags[@]+"${flags[@]}"} "-artifact_prefix=${artifacts}/" "${corpus}" &
    echo "$!" > "${scratch}/pid"
    wait "$!"
    echo "$?" > "${scratch}/status"
  } 2>&1 1>&5 5>&- 6>&- | tee >(
    exec 5>&- 6>&-
    grep -a -F --line-buffered -e "${alarm_line}" -e "${error_line}"
    echo "${end_of_output}"
  ) >&6 5>&- 6>&-
)
exec 5>&- 6>&-

# A read that times out after the ALARM lines, before the end marker, means the fuzzer is still
# running inside its report. Bash 3.2 returns the same status for a read timeout and for end of
# input; the marker is what tells them apart.
stalled=0
while IFS= read -r -u 3 event && [[ "${event}" != "${end_of_output}" ]]; do
  if [[ "${stalled}" == 0 && "${event}" == *"${alarm_line}"* ]] &&
    ! IFS= read -r -u 3 -t "${report_grace_seconds}" event; then
    stalled=1
    kill -KILL "$(cat "${scratch}/pid")" 2> /dev/null
  elif [[ "${event}" == "${end_of_output}" ]]; then
    break
  fi
done
exec 3<&-

if [[ "${stalled}" == 1 ]]; then
  echo "fuzzer-soak: libFuzzer did not finish its timeout report within" \
    "${report_grace_seconds} s of the ALARM line, so it was killed. The timed-out input is the" \
    "unit printed after the ALARM line, if libFuzzer printed it; its timeout- artifact may be" \
    "empty or missing." >&2
  exit "${timeout_status}"
fi
if [[ ! -s "${scratch}/status" ]]; then
  echo "fuzzer-soak: the fuzzer's exit status was not recorded." >&2
  exit 1
fi
exit "$(cat "${scratch}/status")"
