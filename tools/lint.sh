#!/usr/bin/env bash
#
# lint.sh - run the repo-wide banned-source-patterns check.
#
# This is the whole banned-patterns gate. It used to be 476 per-target py_tests
# emitted by the donner_cc_* macros, which cost 714 of the test suite's 2297
# CPU-seconds to do work that takes about 2.5 seconds here, in one process.
#
# The file set is deliberately wider than the macros could see: they enumerated
# only plain-string `srcs`/`hdrs` of targets that `bazel test //...` reached, so
# `select()`-valued sources, label-form sources, `manual`-tagged targets, and
# files no target lists at all were never linted. This scans the trees.
#
# Checks enforced (see docs/coding_style.md "Language and Library Features" and
# build_defs/check_banned_patterns.py): no `long long`, no `std::aligned_storage`,
# no user-defined literal operators, no hidden Unicode whitespace or typographic
# punctuation, and no method above the local decision-point complexity limit.
#
# Usage:
#   tools/lint.sh                 # lint donner/ and examples/
#   tools/lint.sh path [path...]  # lint specific files or directories

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo_root="$(cd "${script_dir}/.." && pwd -P)"
cd "${repo_root}"

readonly kChecker="build_defs/check_banned_patterns.py"

# `render_test.cc` and `wasm_reproducer.cc` are excluded for the same reason the
# Lint workflow excluded them before this script existed.
readonly kExcludedBasenames=(
  "render_test.cc"
  "wasm_reproducer.cc"
)

collect_sources() {
  local root="$1"
  local find_args=("${root}" -type f \( -name '*.cc' -o -name '*.h' \))
  local excluded
  for excluded in "${kExcludedBasenames[@]}"; do
    find_args+=(! -name "${excluded}")
  done
  find "${find_args[@]}"
}

lint_explicit_roots=$#
roots=("$@")
if [[ ${#roots[@]} -eq 0 ]]; then
  roots=(donner examples)
fi

sources=()
for root in "${roots[@]}"; do
  if [[ -f "${root}" ]]; then
    sources+=("${root}")
    continue
  fi
  if [[ ! -d "${root}" ]]; then
    echo "lint.sh: no such file or directory: ${root}" >&2
    exit 1
  fi
  while IFS= read -r path; do
    sources+=("${path}")
  done < <(collect_sources "${root}" | sort)
done

if [[ ${#sources[@]} -eq 0 ]]; then
  echo "lint.sh: no C++ sources found under: ${roots[*]}" >&2
  exit 1
fi

python3 "${kChecker}" "${sources[@]}"

# CodeFactor evaluates complexity only on changed methods. Mirror that locally without making
# existing repository debt block every lint run: explicit paths are checked directly, while the
# default command checks C++ files changed from the branch's upstream plus working-tree additions.
complexity_sources=()
if [[ ${lint_explicit_roots} -gt 0 ]]; then
  complexity_sources=("${sources[@]}")
elif lint_upstream=$(git rev-parse --abbrev-ref --symbolic-full-name '@{upstream}' 2>/dev/null); then
  while IFS= read -r path; do
    if [[ -f "${path}" ]]; then
      complexity_sources+=("${path}")
    fi
  done < <(
    {
      git diff --name-only --diff-filter=ACMR "${lint_upstream}"...HEAD -- \
        '*.cc' '*.cpp' '*.h' '*.hpp' '*.mm'
      git diff --name-only --diff-filter=ACMR HEAD -- '*.cc' '*.cpp' '*.h' '*.hpp' '*.mm'
      git ls-files --others --exclude-standard -- '*.cc' '*.cpp' '*.h' '*.hpp' '*.mm'
    } | sort -u
  )
fi

if [[ ${#complexity_sources[@]} -gt 0 ]]; then
  complexity_args=(--check-method-complexity)
  if [[ -n "${lint_upstream:-}" ]]; then
    complexity_args+=(--complexity-baseline-ref "${lint_upstream}")
  fi
  python3 "${kChecker}" "${complexity_args[@]}" "${complexity_sources[@]}"
fi
