#!/usr/bin/env bash
# Writes "<version>\n<commit>\n". An explicit release SHA avoids reading Git in build actions.
# Without it, developer builds retain their local Git fallback.
set -euo pipefail
if [[ $# -lt 2 || $# -gt 3 ]]; then
  echo "usage: write_build_info.sh MODULE OUTPUT [RELEASE_COMMIT]" >&2
  exit 2
fi
MODULE="$1"
OUT="$2"
COMMIT="${3:-}"
if [[ -n "$COMMIT" && ! "$COMMIT" =~ ^[0-9a-f]{40}$ ]]; then
  echo "release commit must be a full lowercase Git SHA" >&2
  exit 2
fi
VERSION="$(sed -n 's/^[[:space:]]*version[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' "$MODULE" | head -n1)"
if [[ -z "$VERSION" ]]; then
  VERSION="unknown"
fi
if [[ -z "$COMMIT" ]]; then
  if command -v realpath >/dev/null 2>&1; then
    MODULE_REAL="$(realpath "$MODULE")"
  elif [[ -L "$MODULE" ]]; then
    TARGET="$(readlink "$MODULE")"
    case "$TARGET" in
      /*) MODULE_REAL="$TARGET" ;;
      *) MODULE_REAL="$(cd "$(dirname "$MODULE")" && pwd -P)/$TARGET" ;;
    esac
  else
    MODULE_REAL="$(cd "$(dirname "$MODULE")" && pwd -P)/$(basename "$MODULE")"
  fi
  COMMIT="$(git -C "$(dirname "$MODULE_REAL")" rev-parse HEAD 2>/dev/null || echo unknown)"
fi
printf '%s\n%s\n' "$VERSION" "$COMMIT" > "$OUT"
