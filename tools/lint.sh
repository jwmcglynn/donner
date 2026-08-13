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
# punctuation.
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

python3 "${kChecker}" donner "${sources[@]}"
