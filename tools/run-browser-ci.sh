#!/usr/bin/env bash
#
# run-browser-ci.sh - run the editor Wasm browser suites exactly as CI does.
#
# CI-PARITY CONTRACT
# ==================
# This script IS the browser CI job. The lane sequence below is the single
# source of truth for the "Serve Geode package and run browser suites" step of
# the `test` job in .github/workflows/editor_wasm.yml, and that step invokes
# this script. If you change the lanes here, you are changing CI; if you want
# to change CI's lanes, change them here. Do not add a lane to one side only.
#
# The lanes, in order (each is a separate Playwright invocation, exactly as the
# workflow ran them before this script existed):
#
#   1. chromium-default            default playwright.config.js, whole testDir
#                                  except the composited specs it testIgnores
#   2. firefox-geode-resize        compatibility config, that project only
#   3. webkit-geode-carousel       compatibility config, that project only
#   4. firefox-composited-invariants
#                                  playwright.composited-firefox.config.js
#   5. composited-chromium         playwright.composited-chromium.config.js
#
# Differences from the raw workflow shell it replaces, both deliberate:
#   * Lanes no longer fail fast. Every lane runs, each lane's exit code is
#     echoed, and the script exits nonzero if any lane failed. One run reports
#     the whole matrix instead of only the first red lane.
#   * The served bytes are a copy of the built package in a temp directory, not
#     bazel-bin. A later bazel invocation can repoint bazel-bin mid-run and
#     silently serve stale bytes; copying makes that impossible.
#
# Usage:
#   tools/run-browser-ci.sh [extra bazel flags...]
#
# Environment:
#   DONNER_WASM_PACKAGE_DIR  Serve this prebuilt package directory and skip the
#                            bazel build entirely (how CI calls this script: it
#                            downloads the package as an artifact from the
#                            build job).
#   DONNER_BAZEL             bazel binary to use (default: bazelisk, else bazel).
#   DONNER_BAZEL_FLAGS       Extra bazel flags, word-split, applied before any
#                            flags passed as script arguments.
#   DONNER_KEEP_TEMP=1       Do not delete the temp directory on exit.

set -euo pipefail

readonly kPackageTarget="//donner/editor/wasm:wasm_web_package"
readonly kBazelConfig="editor-wasm"

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo_root="$(cd "${script_dir}/.." && pwd -P)"
cd "${repo_root}"

# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------

log() {
  printf '\n=== %s\n' "$*"
}

sha256_of() {
  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  else
    sha256sum "$1" | awk '{print $1}'
  fi
}

# ---------------------------------------------------------------------------
# Cleanup
# ---------------------------------------------------------------------------

server_pid=""
work_dir=""

# shellcheck disable=SC2329  # invoked by the EXIT trap below.
cleanup() {
  if [[ -n "${server_pid}" ]]; then
    kill "${server_pid}" 2>/dev/null || true
    wait "${server_pid}" 2>/dev/null || true
    server_pid=""
  fi
  if [[ -n "${work_dir}" && -d "${work_dir}" ]]; then
    if [[ "${DONNER_KEEP_TEMP:-}" == "1" ]]; then
      echo "Keeping temp directory: ${work_dir}"
    else
      rm -rf "${work_dir}"
    fi
  fi
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# 1. Build (or accept) the Geode editor Wasm package
# ---------------------------------------------------------------------------

extra_bazel_flags=()
if [[ -n "${DONNER_BAZEL_FLAGS:-}" ]]; then
  # Intentional word splitting: this env var carries a flag list.
  # shellcheck disable=SC2206
  extra_bazel_flags=(${DONNER_BAZEL_FLAGS})
fi
if [[ $# -gt 0 ]]; then
  extra_bazel_flags+=("$@")
fi

if [[ -n "${DONNER_WASM_PACKAGE_DIR:-}" ]]; then
  log "Using prebuilt package: ${DONNER_WASM_PACKAGE_DIR}"
  source_pkg_dir="$(cd "${DONNER_WASM_PACKAGE_DIR}" && pwd -P)"
else
  bazel_bin="${DONNER_BAZEL:-}"
  if [[ -z "${bazel_bin}" ]]; then
    if command -v bazelisk >/dev/null 2>&1; then
      bazel_bin="bazelisk"
    else
      bazel_bin="bazel"
    fi
  fi

  log "Building ${kPackageTarget} (--config=${kBazelConfig})"
  "${bazel_bin}" build "--config=${kBazelConfig}" \
    ${extra_bazel_flags[@]+"${extra_bazel_flags[@]}"} "${kPackageTarget}"

  pkg_rel="$("${bazel_bin}" cquery "--config=${kBazelConfig}" --output=files \
    ${extra_bazel_flags[@]+"${extra_bazel_flags[@]}"} "${kPackageTarget}" | head -n1)"
  if [[ -z "${pkg_rel}" || ! -d "${pkg_rel}" ]]; then
    echo "error: could not locate the built package directory (got '${pkg_rel}')" >&2
    exit 1
  fi
  source_pkg_dir="$(cd "${pkg_rel}" && pwd -P)"
fi

# ---------------------------------------------------------------------------
# 2. Copy the package out of bazel-bin
# ---------------------------------------------------------------------------
#
# bazel-bin is a symlink that a later bazel invocation (a different config, a
# concurrent build, an editor plugin refreshing compile commands) can repoint
# underneath a running server. Serving a private copy is the only way to be
# sure the bytes under test are the bytes that were built and hashed.

work_dir="$(mktemp -d "${TMPDIR:-/tmp}/donner-browser-ci.XXXXXX")"
pkg_dir="${work_dir}/package"
server_log="${work_dir}/server.log"
mkdir -p "${pkg_dir}"
cp -RL "${source_pkg_dir}/." "${pkg_dir}/"
chmod -R u+w "${pkg_dir}"

if [[ ! -f "${pkg_dir}/editor.wasm" ]]; then
  echo "error: ${pkg_dir}/editor.wasm is missing; the package copy is not usable" >&2
  exit 1
fi
expected_wasm_sha256="$(sha256_of "${pkg_dir}/editor.wasm")"
log "Serving copy at ${pkg_dir}"
echo "editor.wasm sha256: ${expected_wasm_sha256}"

# ---------------------------------------------------------------------------
# 3. Serve the copy
# ---------------------------------------------------------------------------
#
# simple_webserver.py self-assigns a free port from 8000-8019 and prints
# "Serving at <url>". If every port in that range is taken (a dev server plus a
# few stale runs will do it) it exits with an error, and this falls back to an
# explicit port from a wider range via --port.

start_server() {
  local explicit_port="${1:-}"
  local -a cmd
  cmd=(python3 tools/http/simple_webserver.py --no-https --host 127.0.0.1
       --dir "${pkg_dir}")
  if [[ -n "${explicit_port}" ]]; then
    cmd+=(--port "${explicit_port}")
  fi

  : >"${server_log}"
  PYTHONUNBUFFERED=1 "${cmd[@]}" >"${server_log}" 2>&1 &
  server_pid=$!

  local url=""
  local _i
  for _i in $(seq 1 60); do
    url="$(sed -n 's/^Serving at //p' "${server_log}" | head -n1)"
    if [[ -n "${url}" ]]; then
      break
    fi
    if ! kill -0 "${server_pid}" 2>/dev/null; then
      wait "${server_pid}" 2>/dev/null || true
      server_pid=""
      return 1
    fi
    sleep 1
  done

  if [[ -z "${url}" ]]; then
    return 1
  fi
  printf '%s\n' "${url}" >"${work_dir}/server_url"
  return 0
}

if ! start_server ""; then
  cat "${server_log}" || true
  echo "Default port range unavailable; retrying with an explicit port." >&2
  fallback_port="$(python3 - <<'PY'
import socket
import sys

for port in range(8020, 8200):
    with socket.socket() as probe:
        try:
            probe.bind(("127.0.0.1", port))
        except OSError:
            continue
    print(port)
    sys.exit(0)
sys.exit(1)
PY
)"
  if [[ -z "${fallback_port}" ]]; then
    echo "error: no free TCP port found in 8020-8199" >&2
    exit 1
  fi
  if ! start_server "${fallback_port}"; then
    cat "${server_log}" || true
    echo "error: the package server did not come up" >&2
    exit 1
  fi
fi

server_url="$(cat "${work_dir}/server_url")"
echo "Server URL: ${server_url}"

# ---------------------------------------------------------------------------
# 4. Verify the served bytes are the copied bytes
# ---------------------------------------------------------------------------

curl -fsS -o "${work_dir}/index.html" "${server_url}/index.html"
curl -fsS -o "${work_dir}/served-editor.wasm" "${server_url}/editor.wasm"
served_wasm_sha256="$(sha256_of "${work_dir}/served-editor.wasm")"
if [[ "${served_wasm_sha256}" != "${expected_wasm_sha256}" ]]; then
  echo "error: served editor.wasm does not match the package copy" >&2
  echo "  expected: ${expected_wasm_sha256}" >&2
  echo "  served:   ${served_wasm_sha256}" >&2
  exit 1
fi
echo "Served editor.wasm matches the package copy."
rm -f "${work_dir}/served-editor.wasm"

# ---------------------------------------------------------------------------
# 5. Run every lane, in CI order
# ---------------------------------------------------------------------------

export DONNER_WASM_BACKEND="geode"
export DONNER_WASM_REQUIRE_WEBGPU="1"
export DONNER_WASM_BASE_URL="${server_url}"
# The suites scale their timing bounds by kCiTimeScale when CI is set. Without
# this the local run is measuring different thresholds than CI is.
export CI="${CI:-true}"

readonly kTestsDir="donner/editor/wasm/tests"
if [[ ! -d "${kTestsDir}/node_modules" ]]; then
  log "Installing Playwright test dependencies (npm ci)"
  npm --prefix "${kTestsDir}" ci
fi

# Where each lane's failure evidence is kept.
#
# Every lane writes screenshots, error contexts and attachments into the one
# Playwright output directory, and Playwright empties that directory when it
# starts. A lane therefore erases the evidence of every lane before it, and
# when the last lane passes it erases all of it: the job that produced this
# archive uploaded nothing at all for two failing lanes because the passing
# lane behind them had already cleaned up. Copying each lane's results out as
# soon as the lane ends keeps every lane's evidence, named by the lane that
# produced it.
readonly kResultsDir="${kTestsDir}/test-results"
readonly kFailureArchiveDir="${kTestsDir}/playwright-failures"
rm -rf "${kFailureArchiveDir}"

archive_lane_results() {
  local name="$1"
  if [[ ! -d "${kResultsDir}" ]] || [[ -z "$(ls -A "${kResultsDir}" 2>/dev/null)" ]]; then
    return 0
  fi
  mkdir -p "${kFailureArchiveDir}/${name}"
  cp -R "${kResultsDir}/." "${kFailureArchiveDir}/${name}/"
  echo "Archived ${name} Playwright results to ${kFailureArchiveDir}/${name}"
}

# The lane invocations below spell the tests directory out rather than using
# ${kTestsDir}: they are the definition of the CI lanes, they must read as the
# exact command a human would type, and browser-matrix.spec.mjs matches them
# literally.

lane_names=()
lane_codes=()
overall_status=0

run_lane() {
  local name="$1"
  shift

  log "LANE ${name}"
  printf '%s\n' "  \$ $*"

  local code=0
  "$@" || code=$?

  archive_lane_results "${name}"
  echo "LANE ${name} exit code: ${code}"
  lane_names+=("${name}")
  lane_codes+=("${code}")
  if [[ "${code}" -ne 0 ]]; then
    overall_status=1
  fi
}

# Chromium runs every spec in the tests directory EXCEPT the composited specs,
# which the default config ignores: their per-rAF composited sampler starves
# under SwiftShader, so they run on the platform GPU via their own configs
# below.
run_lane "chromium-default" \
  bash donner/editor/wasm/tests/run_tests.sh --headed

run_lane "firefox-geode-resize" \
  npm --prefix donner/editor/wasm/tests run test:compatibility -- \
  --project=firefox-geode-resize --headed

run_lane "webkit-geode-carousel" \
  npm --prefix donner/editor/wasm/tests run test:compatibility -- \
  --project=webkit-geode-carousel --headed

# Gecko lane for the composited-output invariant suite. It gets its own config
# rather than another compatibility project: that config pins an explicit spec
# list and per-project grep filters, while this suite runs whole and needs a
# longer per-test timeout.
run_lane "firefox-composited-invariants" \
  bash donner/editor/wasm/tests/run_tests.sh --headed \
  --config=playwright.composited-firefox.config.js

# Hardware-Chromium lane for the same suite (full Chromium build on the
# platform GPU; SwiftShader cannot sustain the sampler).
run_lane "composited-chromium" \
  bash donner/editor/wasm/tests/run_tests.sh --headed \
  --config=playwright.composited-chromium.config.js

# ---------------------------------------------------------------------------
# 6. Summary
# ---------------------------------------------------------------------------

log "Browser CI lane summary"
printf '%-34s %s\n' "LANE" "EXIT"
printf '%-34s %s\n' "----------------------------------" "----"
lane_index=0
while [[ "${lane_index}" -lt "${#lane_names[@]}" ]]; do
  printf '%-34s %s\n' "${lane_names[${lane_index}]}" "${lane_codes[${lane_index}]}"
  lane_index=$((lane_index + 1))
done

if [[ "${overall_status}" -ne 0 ]]; then
  echo ""
  echo "Package server log (tail):"
  tail -n 40 "${server_log}" || true
  echo ""
  echo "One or more browser lanes failed."
else
  echo ""
  echo "All browser lanes passed."
fi

exit "${overall_status}"
