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
#
# `examples/` is excluded because `refresh_compile_commands` only covers
# `//donner/...` — example binaries with third-party host-integration
# deps (GLFW, wgpu-native) would otherwise be linted with default flags
# and fail on headers those deps contribute to the include path.
mapfile -t MODIFIED_FILES < <(
  git diff --name-only --diff-filter=ACMR "${BASE}" HEAD \
    | grep -E '\.(cc|h|hpp|cpp)$' \
    | grep -v '^third_party/' \
    | grep -v '^examples/' || true
)

if [[ ${#MODIFIED_FILES[@]} -eq 0 ]]; then
  echo "No modified C++ files; nothing to check."
  exit 0
fi

echo "Refreshing compile_commands.json..."
# `--config=geode` is required so target-gated Geode sources
# (//donner/svg/renderer:renderer_geode et al.) pick up a compile command.
# Without it their target_compatible_with guard makes them invisible to
# hedron_compile_commands, clang-tidy falls back to default flags, and
# headers that depend on the Geode toolchain (webgpu-cpp, wgpu-native)
# fail to resolve.
"${BAZEL}" run --config=geode //tools:refresh_compile_commands

echo "Running misc-include-cleaner on changed lines of ${#MODIFIED_FILES[@]} file(s):"
printf '  %s\n' "${MODIFIED_FILES[@]}"

# clang-tidy-diff.py reads a unified diff on stdin and invokes clang-tidy
# with --line-filter matching only the added/modified lines, so findings on
# pre-existing code in touched files do not fail the gate.
#
#   -p1           : strip one path component (matches `git diff` output)
#   -path .       : look for compile_commands.json in the current dir
#   -iregex       : only consider C/C++ source files
#   -checks       : restrict to misc-include-cleaner; .clang-tidy's
#                   WarningsAsErrors: "*" still applies to whatever
#                   remains enabled, so any new finding fails.
#   -clang-tidy-binary : pin to our clang-tidy-19
#
# Trailing `-- <args>` is intentionally omitted: that path forwards flags
# to the compiler, not clang-tidy, and clang-tidy-diff.py has no pass-
# through for clang-tidy options except via `-checks` above.
git diff --unified=0 "${BASE}" HEAD \
    -- '*.cc' '*.h' '*.hpp' '*.cpp' ':!third_party/' ':!examples/' \
  | "${CLANG_TIDY_DIFF}" \
      -p1 \
      -path . \
      -iregex '.*\.(cc|h|hpp|cpp)$' \
      -checks='-*,misc-include-cleaner' \
      -clang-tidy-binary "${CLANG_TIDY}"
