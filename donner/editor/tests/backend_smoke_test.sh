#!/bin/bash
# Runs `//donner/editor:editor --backend-smoke-test` and asserts that the
# session-sandboxed transport is wired up correctly — the editor spawns
# `donner_editor_backend` as a child process, exchanges a handshake, and
# reports a distinct backend PID.
#
# Ships on both Linux and macOS so a regression where main.cc or
# BUILD.bazel accidentally falls back to the in-process backend gets
# caught immediately.

set -euo pipefail

EDITOR="${TEST_SRCDIR}/_main/donner/editor/editor"

if [[ ! -x "${EDITOR}" ]]; then
  echo "FAIL: editor binary not found at ${EDITOR}" >&2
  exit 1
fi

OUTPUT="$("${EDITOR}" --backend-smoke-test 2>&1)"
STATUS=$?

echo "${OUTPUT}"

if [[ ${STATUS} -ne 0 ]]; then
  echo "FAIL: editor exited with status ${STATUS}" >&2
  exit 1
fi

if ! grep -q "smoke-test: transport=session status=ok" <<<"${OUTPUT}"; then
  echo "FAIL: output missing 'status=ok' marker" >&2
  exit 1
fi

# Extract host/backend pids and assert they differ.
HOST_PID="$(grep -oE 'host_pid=[0-9]+' <<<"${OUTPUT}" | head -1 | cut -d= -f2)"
BACKEND_PID="$(grep -oE 'backend_pid=[0-9]+' <<<"${OUTPUT}" | head -1 | cut -d= -f2)"

if [[ -z "${HOST_PID}" || -z "${BACKEND_PID}" ]]; then
  echo "FAIL: could not parse host_pid/backend_pid from output" >&2
  exit 1
fi

if [[ "${HOST_PID}" == "${BACKEND_PID}" ]]; then
  echo "FAIL: host_pid (${HOST_PID}) == backend_pid (${BACKEND_PID})" >&2
  echo "     — the backend is running in the editor process, not sandboxed" >&2
  exit 1
fi

echo "PASS: host_pid=${HOST_PID} backend_pid=${BACKEND_PID} (distinct)"
