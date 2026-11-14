#!/usr/bin/env bash
#
# bazel_watchdog.sh - run one Bazel command with a heartbeat and a stall watchdog, teeing its
# console output to a log.
#
# Usage: tools/ci/bazel_watchdog.sh LOG_PATH COMMAND [ARGS...]
#
# Exits with COMMAND's status. BAZEL_STALL_LIMIT_SECONDS (default 900) and
# BAZEL_HEARTBEAT_INTERVAL_SECONDS (default 60) tune the watchdog.
#
# The heartbeat alone is not diagnosis: a run can print "still running for 12190s" 200 times
# while Bazel has been parked on one target the whole time. Nothing else catches that - a
# per-test timeout only starts counting once a test RUNS, so a target that is queued and never
# scheduled is bounded by nothing but the job's own `timeout-minutes`, which kills the run without
# saying which target it was waiting on.
#
# The watchdog watches Bazel's own progress output. Bazel re-emits its progress line with updated
# elapsed time while an action is in flight, so a log whose last Bazel-authored line has not
# changed for BAZEL_STALL_LIMIT_SECONDS means the invocation has stopped making observable
# progress. When that happens, print that line as an error (it names the target and its state,
# e.g. `[Sched] Testing //path:target (shard 1 of 16)`), save it as stall.txt beside LOG_PATH, and
# fail immediately instead of burning the rest of the job budget in silence.
set -euo pipefail

log_path="$1"
shift
started=$SECONDS
stall_limit="${BAZEL_STALL_LIMIT_SECONDS:-900}"
heartbeat_interval="${BAZEL_HEARTBEAT_INTERVAL_SECONDS:-60}"
stall_report="$(dirname "$log_path")/stall.txt"
heartbeat_control="${log_path}.heartbeat-control.$$"
mkfifo "$heartbeat_control"
exec 9<> "$heartbeat_control"

# Killing only the launcher is not enough: bazelisk forks the Bazel
# client, which keeps the write end of this pipeline open, so `tee`
# never sees EOF and the step hangs anyway - the watchdog would report
# the stall and then stall itself. Signal the whole descendant tree.
# (Process-group signalling is not reliable here: this block already
# runs as one stage of a pipeline, so `set -m` does not consistently
# give the background job a group of its own.)
child_processes() {
  local root="$1" children="" child parent
  if [[ -r "/proc/$root/task/$root/children" ]]; then
    IFS= read -r children < "/proc/$root/task/$root/children" || true
    for child in $children; do
      printf '%s\n' "$child"
    done
    return
  fi
  while read -r child parent; do
    if [[ "$parent" == "$root" ]]; then
      printf '%s\n' "$child"
    fi
  done < <(ps -A -o pid=,ppid= 2>/dev/null)
}

kill_tree() {
  local signal="$1" root="$2" child
  while IFS= read -r child; do
    [[ -n "$child" ]] || continue
    kill_tree "$signal" "$child"
  done < <(child_processes "$root")
  kill "-$signal" "$root" 2>/dev/null || true
}

set +e
{
  set -e
  "$@" &
  command_pid=$!

  (
    last_progress=""
    last_change=$SECONDS

    while kill -0 "$command_pid" 2>/dev/null; do
      if IFS= read -r -t "$heartbeat_interval" _ <&9; then
        exit 0
      fi
      kill -0 "$command_pid" 2>/dev/null || break

      # Ignore our own annotations so the watchdog cannot see its own
      # output as progress.
      current="$(grep -v '^::' "$log_path" 2>/dev/null | tail -n 1 || true)"
      if [[ "$current" != "$last_progress" ]]; then
        last_progress="$current"
        last_change=$SECONDS
        echo "::notice::Bazel command still running for $((SECONDS - started))s"
        continue
      fi

      stalled=$((SECONDS - last_change))
      if (( stalled < stall_limit )); then
        echo "::notice::Bazel command still running for $((SECONDS - started))s (no new progress for ${stalled}s)"
        continue
      fi

      {
        echo "Bazel made no observable progress for ${stalled}s (limit ${stall_limit}s)."
        echo "Last Bazel progress line, which names what it was waiting on:"
        echo "$last_progress"
      } | tee "$stall_report"
      echo "::error::Bazel stalled for ${stalled}s on: ${last_progress:-<no progress output>}"
      kill_tree TERM "$command_pid"
      sleep 15
      kill_tree KILL "$command_pid"
      break
    done
  ) &
  heartbeat_pid=$!

  set +e
  wait "$command_pid"
  status=$?
  set -e

  printf 'stop\n' >&9
  wait "$heartbeat_pid" 2>/dev/null || true
  exit "$status"
} 2>&1 | tee "$log_path"
status="${PIPESTATUS[0]}"
set -e
exec 9>&-
rm -f "$heartbeat_control"
exit "$status"
