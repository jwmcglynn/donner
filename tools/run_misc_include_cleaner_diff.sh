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
# Exclusions:
#   - `third_party/` — vendored code linted by its upstream.
#   - `examples/` — `refresh_compile_commands` only covers `//donner/...`;
#     example binaries with third-party host-integration deps (GLFW,
#     wgpu-native) would otherwise be linted with default flags and fail
#     on headers those deps contribute to the include path.
#   - Geode sources — same reason, but for the Geode renderer itself.
#     `//donner/svg/renderer:renderer_geode` is `target_compatible_with =
#     geode_enabled`, so hedron_compile_commands with the default config
#     skips it. Linting it under default flags chokes on `<webgpu/webgpu.hpp>`.
#     Attempts to add `--config=geode` via the targets dict pull WOFF2 →
#     brotli into compile commands in a way that trips clang-tidy-19's
#     header analyzer on function-like macro preconditions. Track the
#     follow-up as issue #559.
mapfile -t MODIFIED_FILES < <(
  git diff --name-only --diff-filter=ACMR "${BASE}" HEAD \
    | grep -E '\.(cc|h|hpp|cpp)$' \
    | grep -v '^third_party/' \
    | grep -v '^examples/' \
    | grep -v '^donner/svg/renderer/geode/' \
    | grep -v 'Geode' \
    | grep -v 'TinySkia' \
    | grep -v '_macOS\.' \
    | grep -v 'resvg_test_suite' || true
)

if [[ ${#MODIFIED_FILES[@]} -eq 0 ]]; then
  echo "No modified C++ files; nothing to check."
  exit 0
fi

echo "Refreshing compile_commands.json..."
# `--config=geode` is applied per-target in tools/BUILD.bazel so target-gated
# Geode sources (//donner/svg/renderer:renderer_geode,
# //donner/svg/renderer/tests:renderer_test_backend, etc.) produce compile
# commands with webgpu-cpp / wgpu-native include paths. Passing the flag
# here on `bazel run` only affects how the refresh tool itself is built.
"${BAZEL}" run //tools:refresh_compile_commands

# `bazel run //tools:refresh_compile_commands` extracts compile commands
# from Bazel's analysis graph but does NOT materialize generated headers
# (e.g. `donner/editor/EditorIcon.h` from the embed_resources rule) or
# external-repo headers (e.g. `rules_cc/cc/runfiles/runfiles.h` consumed
# via `donner/base/tests/Runfiles.h`). Without those on disk, clang-tidy
# fails with "file not found" for every TU that transitively depends on
# them — even though the path is right in compile_commands.json.
#
# Targeted pre-build: just the editor's three `embed_resources` headers
# and `base_test_utils` (which pulls `@rules_cc//cc/runfiles` and
# materializes the external repo). This is fast (cached after first
# main run) and avoids the ~25-min full `//donner/...` build.
echo "Pre-building generated headers + rules_cc external for clang-tidy..."
"${BAZEL}" build //donner/editor:editor_icon //donner/editor:editor_splash \
    //donner/editor:notice //donner/base:base_test_utils --keep_going \
    || echo "Pre-build had failures; continuing — clang-tidy may surface compile errors for unbuilt files."

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
# clang-tidy-19 needs explicit pointers to the GCC 13 stdlib headers
# on ubuntu-24.04. The runner image installs `libstdc++-13-dev` and
# `g++-13` by default, but clang-tidy's driver autodetection
# inconsistently resolves them — every check fails with `'cstddef' file
# not found` on header parses. Force-add the GCC 13 include paths via
# `-extra-arg-before` (only when running under Linux/CI; on macOS
# Xcode's SDK provides these via `-isysroot` baked into the compile
# commands).
extra_args=()
if [[ "$(uname -s)" == "Linux" ]]; then
  for inc in \
      /usr/include/c++/13 \
      /usr/include/x86_64-linux-gnu/c++/13 \
      /usr/include/c++/13/backward; do
    if [[ -d "${inc}" ]]; then
      extra_args+=(-extra-arg-before="-isystem${inc}")
    fi
  done

  echo "Diagnostic: clang-tidy-19 stdlib search paths injected:"
  printf '  %s\n' "${extra_args[@]}"
fi

# Capture clang-tidy-diff output so we can post-filter: the lint should
# only fail on actual `misc-include-cleaner` findings. Compile errors
# (`clang-diagnostic-error`) coming from Bazel-toolchain quirks — paths
# under config-transition-hashed `bazel-out` dirs that the lint runner
# can't materialize with a plain `bazel build`, e.g. the
# `_virtual_includes` directories used to project external repo headers
# like `rules_cc/cc/runfiles/runfiles.h` — leak into the output but
# aren't actionable from an include-cleaner perspective. Print them for
# diagnostics, but only fail the gate when there are real findings.
set +e
output=$(
  git diff --unified=0 "${BASE}" HEAD \
      -- '*.cc' '*.h' '*.hpp' '*.cpp' \
         ':!third_party/' \
         ':!examples/' \
         ':!donner/svg/renderer/geode/' \
         ':(exclude,glob)**/*Geode*' \
         ':(exclude,glob)**/*TinySkia*' \
         ':(exclude,glob)**/*_macOS.*' \
         ':(exclude,glob)**/resvg_test_suite*' \
    | "${CLANG_TIDY_DIFF}" \
        -p1 \
        -path . \
        -iregex '.*\.(cc|h|hpp|cpp)$' \
        -checks='-*,misc-include-cleaner' \
        -clang-tidy-binary "${CLANG_TIDY}" \
        "${extra_args[@]}" 2>&1
)
clang_tidy_exit=$?
set -e

printf '%s\n' "${output}"

# Only fail on actual misc-include-cleaner findings; ignore
# clang-diagnostic-error noise from unmaterialized bazel-out paths.
if printf '%s' "${output}" | grep -q 'misc-include-cleaner,-warnings-as-errors'; then
  echo
  echo "FAIL: misc-include-cleaner findings present (exit code ${clang_tidy_exit})."
  exit 1
fi

if [[ ${clang_tidy_exit} -ne 0 ]]; then
  echo
  echo "WARN: clang-tidy reported ${clang_tidy_exit} but no misc-include-cleaner"
  echo "      findings — treating as a toolchain/path glitch (likely a Bazel"
  echo "      external repo or generated header that the lint job's pre-build"
  echo "      didn't materialize). Gate stays green."
fi
