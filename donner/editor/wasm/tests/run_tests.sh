#!/usr/bin/env bash
#
# Drives `npx playwright test` against the Donner WASM editor, launching the
# `serve_http` binary in the background. Designed to be invoked from Bazel
# (`bazel run //donner/editor/wasm/tests:playwright_tests`) but also works
# standalone once Playwright + Chromium are installed in-repo.
#
# Required tools in PATH: node, npx, bazel. (We deliberately shell out to
# `bazel run` for serve_http rather than re-implementing the serve logic —
# that's the same server humans use when they click around manually, so
# anything the harness catches will reproduce under `bazel run serve_http`
# + F5.)
#
# Environment knobs (all optional):
#   DONNER_WASM_BASE_URL     Skip bazel-serve and target an already-running
#                            server at this URL. (Implies SKIP_WEBSERVER=1.)
#   DONNER_WASM_SKIP_BUILD   Don't rebuild //donner/editor/wasm:wasm first.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# If invoked via Bazel, BUILD_WORKSPACE_DIRECTORY points at the repo root.
REPO_ROOT="${BUILD_WORKSPACE_DIRECTORY:-}"
if [[ -z "${REPO_ROOT}" ]]; then
  REPO_ROOT="$(cd "${SCRIPT_DIR}/../../../.." && pwd)"
fi

cd "${REPO_ROOT}"

log() { printf "[wasm-e2e] %s\n" "$*" >&2; }

# 1. Build the WASM bundle (unless opted out).
if [[ "${DONNER_WASM_SKIP_BUILD:-0}" != "1" ]]; then
  log "Building //donner/editor/wasm:wasm_web_package ..."
  bazel build --config=editor-wasm //donner/editor/wasm:wasm_web_package >&2
fi

# 2. Install node_modules for the test package if needed. We keep the
#    `node_modules` directory under the tests/ folder — gitignored, shared
#    by local runs and Bazel invocations.
TEST_DIR="${REPO_ROOT}/donner/editor/wasm/tests"
cd "${TEST_DIR}"

if [[ ! -d node_modules/@playwright/test ]]; then
  log "Installing @playwright/test ..."
  npm install --no-audit --no-fund >&2
fi

# 3. Ensure Chromium is present. Playwright caches it under
#    ~/Library/Caches/ms-playwright on macOS and ~/.cache/ms-playwright on
#    Linux; `playwright install` is idempotent + fast when it's already
#    cached.
log "Ensuring Chromium is installed ..."
npx playwright install chromium >&2

# 4. Start the bazel-served editor in the background unless the caller
#    already pointed us at a running one.
SERVE_PID=""
cleanup() {
  if [[ -n "${SERVE_PID}" ]] && kill -0 "${SERVE_PID}" 2>/dev/null; then
    log "Stopping serve_http (pid ${SERVE_PID}) ..."
    kill "${SERVE_PID}" 2>/dev/null || true
    wait "${SERVE_PID}" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

if [[ -z "${DONNER_WASM_BASE_URL:-}" ]]; then
  SERVE_LOG="$(mktemp -t donner-wasm-serve.XXXXXX)"
  log "Starting serve_http (log: ${SERVE_LOG}) ..."
  # `--no-https` avoids the local-CA prompt; Playwright is happy talking to
  # plain-HTTP localhost (which is a secure context for SharedArrayBuffer).
  (
    cd "${REPO_ROOT}"
    bazel run --config=editor-wasm //donner/editor/wasm:serve_http -- --no-https
  ) >"${SERVE_LOG}" 2>&1 &
  SERVE_PID=$!

  # Wait for the "Serving at http://..." line and capture the URL.
  BASE_URL=""
  for _ in $(seq 1 120); do
    if grep -Eo 'Serving at http://[^ ]+' "${SERVE_LOG}" >/dev/null 2>&1; then
      BASE_URL="$(grep -Eo 'Serving at http://[^ ]+' "${SERVE_LOG}" | head -n 1 | sed 's|^Serving at ||')"
      break
    fi
    if ! kill -0 "${SERVE_PID}" 2>/dev/null; then
      log "serve_http exited early; dumping log:"
      cat "${SERVE_LOG}" >&2
      exit 1
    fi
    sleep 0.5
  done
  if [[ -z "${BASE_URL}" ]]; then
    log "Timed out waiting for serve_http startup; dumping log:"
    cat "${SERVE_LOG}" >&2
    exit 1
  fi
  export DONNER_WASM_BASE_URL="${BASE_URL}"
  log "serve_http ready at ${BASE_URL}"
fi

# 5. Disable Playwright's own webServer block — we manage serve_http.
export DONNER_WASM_SKIP_WEBSERVER=1

# 6. Run the tests.
log "Running Playwright tests against ${DONNER_WASM_BASE_URL} ..."
cd "${TEST_DIR}"
exec npx playwright test "$@"
