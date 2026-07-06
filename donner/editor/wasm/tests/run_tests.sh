#!/usr/bin/env bash
set -euo pipefail

# Base URL of the served editor package. Defaults to the local dev server.
# Override with DONNER_WASM_BASE_URL or, backward-compatibly, a leading
# http(s):// positional argument so a CI lane can point at whichever package it
# served. Any other arguments are forwarded to `playwright test` unchanged.
if [[ $# -gt 0 && "$1" == http://* || $# -gt 0 && "$1" == https://* ]]; then
  export DONNER_WASM_BASE_URL="$1"
  shift
fi
export DONNER_WASM_BASE_URL="${DONNER_WASM_BASE_URL:-http://127.0.0.1:8000}"

# Renderer backend of the served package ("geode" or "tiny_skia"). Inherited
# from the environment; the smoke suite adjusts its WebGPU gating accordingly.
export DONNER_WASM_BACKEND="${DONNER_WASM_BACKEND:-geode}"

cd "$(dirname "$0")"
npx playwright test "$@"
