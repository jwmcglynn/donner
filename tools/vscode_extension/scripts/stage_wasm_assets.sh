#!/usr/bin/env bash
# stage_wasm_assets.sh — Copies Donner WASM build output into media/generated/
# for the VS Code extension.
#
# Usage:
#   ./scripts/stage_wasm_assets.sh [bazel_workspace_root]
#
# If no argument is provided, the script expects to be run from the repository root
# or discovers it via `bazel info workspace`.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXTENSION_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
GENERATED_DIR="$EXTENSION_DIR/media/generated"

if [[ $# -ge 1 ]]; then
  WORKSPACE_ROOT="$1"
else
  WORKSPACE_ROOT="$(bazel info workspace 2>/dev/null || cd "$EXTENSION_DIR/../.." && pwd)"
fi

WASM_OUT="$WORKSPACE_ROOT/bazel-bin/donner/svg/renderer/wasm"

if [[ ! -d "$WASM_OUT" ]]; then
  echo "ERROR: WASM build output not found at $WASM_OUT"
  echo "Build it first with:"
  echo "  bazel build //donner/svg/renderer/wasm:donner_wasm"
  exit 1
fi

mkdir -p "$GENERATED_DIR"

# Copy .wasm and .js loader files.
for ext in wasm js; do
  for f in "$WASM_OUT"/*."$ext"; do
    if [[ -f "$f" ]]; then
      cp -v "$f" "$GENERATED_DIR/"
    fi
  done
done

echo "WASM assets staged to $GENERATED_DIR/"
