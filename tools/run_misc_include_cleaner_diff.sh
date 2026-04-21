#!/bin/bash
#
# Run clang-tidy's misc-include-cleaner only on the LINES added or modified
# in the current diff. Uses clang-tidy-diff.py so findings on pre-existing
# code that happens to live in a touched file are not reported — matches
# the "diff-only enforcement" contract from doc 0031 M1.1.
#
# Historical debt is tracked in issue #559.

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

if command -v clang-tidy-diff-19.py >/dev/null 2>&1; then
  CLANG_TIDY_DIFF="clang-tidy-diff-19.py"
elif command -v clang-tidy-diff.py >/dev/null 2>&1; then
  CLANG_TIDY_DIFF="clang-tidy-diff.py"
else
  echo "clang-tidy-diff[-19].py not found in PATH" >&2
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

# Quick pre-check: if the diff touches zero C++ files, skip the expensive
# compile_commands refresh and exit clean.
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

echo "Running misc-include-cleaner on changed lines of ${#MODIFIED_FILES[@]} file(s):"
printf '  %s\n' "${MODIFIED_FILES[@]}"

# clang-tidy-diff.py reads a unified diff on stdin and invokes clang-tidy
# with --line-filter matching only the added/modified lines, so findings on
# pre-existing code in touched files do not fail the gate.
#
#   -p1           : strip one path component (matches `git diff` output)
#   -path .       : look for compile_commands.json in the current dir
#   -iregex       : only consider C/C++ source files
#   -clang-tidy-binary : pin to our clang-tidy-19
#   -- <args>     : everything after -- is forwarded to clang-tidy
#
# We pass --warnings-as-errors so any new finding still fails CI.
git diff --unified=0 "${BASE}" HEAD -- '*.cc' '*.h' '*.hpp' '*.cpp' ':!third_party/' \
  | "${CLANG_TIDY_DIFF}" \
      -p1 \
      -path . \
      -iregex '.*\.(cc|h|hpp|cpp)$' \
      -clang-tidy-binary "${CLANG_TIDY}" \
      -- \
      --checks=-*,misc-include-cleaner \
      --warnings-as-errors=*
