#!/bin/bash
#
# Run clang-tidy's misc-include-cleaner only on changed C++ files.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPO_ROOT}"

BASE="${1:-}"
if [[ -z "${BASE}" ]]; then
  git fetch --depth=50 origin main >/dev/null 2>&1 || true
  BASE="$(git merge-base origin/main HEAD 2>/dev/null || echo HEAD~1)"
fi

if command -v clang-tidy-19 >/dev/null 2>&1; then
  CLANG_TIDY="clang-tidy-19"
elif command -v clang-tidy >/dev/null 2>&1; then
  CLANG_TIDY="clang-tidy"
else
  echo "clang-tidy not found in PATH" >&2
  exit 1
fi

if command -v bazelisk >/dev/null 2>&1; then
  BAZEL="bazelisk"
elif command -v bazel >/dev/null 2>&1; then
  BAZEL="bazel"
else
  echo "bazelisk/bazel not found in PATH" >&2
  exit 1
fi

mapfile -t MODIFIED_FILES < <(
  git diff --name-only --diff-filter=ACMR "${BASE}" HEAD \
    | grep -E '\.(cc|h|hpp|cpp)$' \
    | grep -v '^third_party/' || true
)

if [[ ${#MODIFIED_FILES[@]} -eq 0 ]]; then
  echo "No modified C++ files; nothing to check."
  exit 0
fi

echo "Refreshing compile_commands.json..."
"${BAZEL}" run //tools:refresh_compile_commands

echo "Running misc-include-cleaner on ${#MODIFIED_FILES[@]} changed file(s):"
printf '  %s\n' "${MODIFIED_FILES[@]}"

"${CLANG_TIDY}" \
  -p . \
  --quiet \
  --checks=-*,misc-include-cleaner \
  --warnings-as-errors=* \
  "${MODIFIED_FILES[@]}"
