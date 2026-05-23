#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
export DONNER_WASM_BASE_URL="${DONNER_WASM_BASE_URL:-http://127.0.0.1:8000}"
npx playwright test "$@"
