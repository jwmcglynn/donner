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
#
# The per-input timeout is a hang guard with room for a stalled runner and for a cold process's
# first inputs, so it cannot also catch an input that is merely slow. libFuzzer reports a unit that
# took at least -report_slow_units whole seconds, and more than 10% longer than the longest unit it
# already reported (seeds included), with a "Slowest unit" line; it saves the unit as a slow-unit-
# artifact and keeps fuzzing. libFuzzer times a unit by wall clock, so a runner that stops
# scheduling the process produces the same report. For every report that arrives after INITED, that
# is while mutating, the launcher therefore replays the saved unit once in a fresh process, under
# the same relay and watchdog as the fuzzer itself. The soak fails with the timeout status when a
# replay is slow too or prints no time, or when libFuzzer saved no unit to replay, and with the
# replay's own status when a replay fails; a fast replay is reported as a stall. Seed replay before
# INITED is the process's warm-up and keeps the hang guard's headroom; the corpus regression test
# replays those seeds on every change. The gate needs libFuzzer's INITED and "Slowest unit" lines on
# this launcher's stderr, so it does not apply under -verbosity=0 or the -fork, -jobs and -merge
# modes, none of which the soak targets use.
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
# libFuzzer's stats line after the seed corpus replay, for example "#5<TAB>INITED cov: ...".
readonly inited_line=$'\tINITED '
readonly slow_unit_line='Slowest unit: '
# Not anchored: target output without a final newline can put the report mid-line. A forged report
# can only add a replay or fail the soak, never pass it.
readonly slow_unit_pattern='Slowest unit: [0-9]+ s:'
# libFuzzer names the file it saved right after a slow-unit report:
# "artifact_prefix='<prefix>'; Test unit written to <prefix>slow-unit-<sha1>".
readonly written_line='Test unit written to '
# libFuzzer's timing line when it runs individual files: "Executed <file> in <N> ms".
readonly executed_line='Executed '
readonly executed_pattern='^Executed .* in ([0-9]+) ms$'
readonly end_of_output='fuzzer-soak: end of fuzzer output'
# A healthy report prints its error line milliseconds after the ALARM lines.
readonly report_grace_seconds=5
# libFuzzer's default -timeout_exitcode.
readonly timeout_status=70

# Runs the binary with the given arguments, relaying its output unchanged and watching its stderr.
# Sets run_status to its exit status, or to the timeout status after a stalled timeout report;
# slow_units to the artifacts of slow-unit reports after INITED, one per line; unsaved_reports to
# the number of those reports without a saved artifact; and executed_ms to the time in its last
# "Executed" line.
#
# The relay uses only anonymous pipes: on macOS a FIFO reader can miss end of file after its last
# writer exits, and bash 3.2 does not retry a FIFO open() that SIGCHLD interrupts. tee copies the
# binary's stderr to this launcher's stderr and to grep, whose matches and final end marker arrive
# on fd 3. The binary runs inside that process substitution, so its pid and status come back
# through files; the status is written before the end marker can arrive.
run_watched() {
  rm -f "${scratch}/pid" "${scratch}/status"
  exec 5>&1 6>&2
  exec 3< <(
    {
      "${binary}" "$@" &
      echo "$!" > "${scratch}/pid"
      wait "$!"
      echo "$?" > "${scratch}/status"
    } 2>&1 1>&5 5>&- 6>&- | tee >(
      exec 5>&- 6>&-
      grep -a -F --line-buffered -e "${alarm_line}" -e "${error_line}" -e "${inited_line}" \
        -e "${slow_unit_line}" -e "${written_line}" -e "${executed_line}"
      echo "${end_of_output}"
    ) >&6 5>&- 6>&-
  )
  exec 5>&- 6>&-

  # A read that times out after the ALARM lines, before the end marker, means the binary is still
  # running inside its report. Bash 3.2 returns the same status for a read timeout and for end of
  # input; the marker is what tells them apart.
  local event stalled=0 mutating=0 awaiting_artifact=0 artifact
  slow_units=""
  unsaved_reports=0
  executed_ms=""
  while IFS= read -r -u 3 event && [[ "${event}" != "${end_of_output}" ]]; do
    if [[ "${event}" == *"${inited_line}"* ]]; then
      mutating=1
    elif [[ "${mutating}" == 1 && "${event}" =~ ${slow_unit_pattern} ]]; then
      unsaved_reports=$((unsaved_reports + awaiting_artifact))
      awaiting_artifact=1
    elif [[ "${awaiting_artifact}" == 1 && "${event}" == *"${written_line}"* ]]; then
      artifact="${event##*"${written_line}"}"
      # Only a file libFuzzer saved under this soak's prefix is replayed.
      if [[ "${artifact}" == "${artifacts}/slow-unit-"* && -f "${artifact}" ]]; then
        slow_units+="${artifact}"$'\n'
      else
        unsaved_reports=$((unsaved_reports + 1))
      fi
      awaiting_artifact=0
    elif [[ "${event}" =~ ${executed_pattern} ]]; then
      executed_ms="${BASH_REMATCH[1]}"
    fi
    # The line read during the grace period is the next line of libFuzzer's timeout report, usually
    # its timeout- artifact line or its error line. libFuzzer prints no INITED, slow-unit or
    # Executed line inside a timeout report, so nothing this loop records is skipped.
    if [[ "${stalled}" == 0 && "${event}" == *"${alarm_line}"* ]] &&
      ! IFS= read -r -u 3 -t "${report_grace_seconds}" event; then
      stalled=1
      kill -KILL "$(cat "${scratch}/pid")" 2> /dev/null
    elif [[ "${event}" == "${end_of_output}" ]]; then
      break
    fi
  done
  exec 3<&-
  unsaved_reports=$((unsaved_reports + awaiting_artifact))

  if [[ "${stalled}" == 1 ]]; then
    echo "fuzzer-soak: libFuzzer did not finish its timeout report within" \
      "${report_grace_seconds} s of the ALARM line, so it was killed. The timed-out input is the" \
      "unit printed after the ALARM line, if libFuzzer printed it; its timeout- artifact may be" \
      "empty or missing." >&2
    run_status="${timeout_status}"
  elif [[ ! -s "${scratch}/status" ]]; then
    echo "fuzzer-soak: the fuzzer's exit status was not recorded." >&2
    run_status=1
  else
    run_status="$(cat "${scratch}/status")"
  fi
}

# Staging above runs under errexit, so a seed that cannot be copied fails the run rather than
# silently shrinking the corpus. The fuzzer's status is the launcher's, including 128+N when a
# signal kills it; only a stalled timeout report or a slow replay replaces a successful status.
set +e
run_watched ${flags[@]+"${flags[@]}"} "-artifact_prefix=${artifacts}/" "${corpus}"
if [[ "${run_status}" != 0 ]]; then
  exit "${run_status}"
fi
if [[ "${unsaved_reports}" != 0 ]]; then
  echo "fuzzer-soak: libFuzzer reported a slow unit after the seed corpus replay but saved no" \
    "slow-unit- artifact to replay." >&2
  exit "${timeout_status}"
fi

slow_seconds=10
replay_flags=()
for flag in ${flags[@]+"${flags[@]}"}; do
  case "${flag}" in
    -report_slow_units=*) slow_seconds="${flag#*=}" ;;
    -timeout=*) replay_flags=("${flag}") ;;
  esac
done
reported_units=()
while IFS= read -r unit; do
  if [[ -n "${unit}" ]]; then
    reported_units+=("${unit}")
  fi
done <<< "${slow_units}"
for unit in ${reported_units[@]+"${reported_units[@]}"}; do
  echo "fuzzer-soak: replaying ${unit##*/} once to check whether the unit itself is slow." >&2
  run_watched ${replay_flags[@]+"${replay_flags[@]}"} "-artifact_prefix=${artifacts}/replay-" \
    -runs=1 "${unit}"
  if [[ "${run_status}" != 0 ]]; then
    echo "fuzzer-soak: libFuzzer reported a slow unit after the seed corpus replay, and" \
      "replaying ${unit##*/} failed with status ${run_status}." >&2
    exit "${run_status}"
  fi
  if [[ -z "${executed_ms}" || "${executed_ms}" -ge $((slow_seconds * 1000)) ]]; then
    echo "fuzzer-soak: libFuzzer reported a slow unit after the seed corpus replay, and" \
      "replaying ${unit##*/} took ${executed_ms:-an unknown number of} ms. The unit is in the" \
      "findings directory." >&2
    exit "${timeout_status}"
  fi
  echo "fuzzer-soak: libFuzzer reported ${unit##*/} as a slow unit, but its replay took" \
    "${executed_ms} ms, so the process stalled rather than the input being slow." >&2
done
exit 0
