#!/usr/bin/env bash
# Writes "<version>\n<commit>\n" to the output file.
#
# The version string is read from MODULE.bazel, the repo's single source of
# truth for the Donner version. The commit is the current git HEAD, falling
# back to "unknown" when git is unavailable (e.g. a sandboxed or bare build).
#
# Usage: write_build_info.sh <path/to/MODULE.bazel> <output>
set -euo pipefail

MODULE="$1"
OUT="$2"

# Resolve the MODULE.bazel symlink in the Bazel execroot to the real source
# tree so `git` can find the checkout's `.git`.
if command -v realpath >/dev/null 2>&1; then
  MODULE_REAL="$(realpath "$MODULE")"
elif [ -L "$MODULE" ]; then
  TARGET="$(readlink "$MODULE")"
  case "$TARGET" in
    /*) MODULE_REAL="$TARGET" ;;
    *) MODULE_REAL="$(cd "$(dirname "$MODULE")" && pwd -P)/$TARGET" ;;
  esac
else
  MODULE_REAL="$(cd "$(dirname "$MODULE")" && pwd -P)/$(basename "$MODULE")"
fi
WS="$(dirname "$MODULE_REAL")"

VERSION="$(sed -n 's/^[[:space:]]*version[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' "$MODULE" | head -n1)"
if [ -z "$VERSION" ]; then
  VERSION="unknown"
fi
COMMIT="$(git -C "$WS" rev-parse HEAD 2>/dev/null || echo unknown)"

printf '%s\n%s\n' "$VERSION" "$COMMIT" > "$OUT"
