#!/usr/bin/env bash
#
# lint.sh - run repo-wide source and formatting checks.
#
# The banned-patterns check used to be 476 per-target py_tests
# emitted by the donner_cc_* macros, which cost 714 of the test suite's 2297
# CPU-seconds to do work that takes about 2.5 seconds here, in one process.
#
# The file set is deliberately wider than the macros could see: they enumerated
# only plain-string `srcs`/`hdrs` of targets that `bazel test //...` reached, so
# `select()`-valued sources, label-form sources, `manual`-tagged targets, and
# files no target lists at all were never linted. This scans the trees.
#
# Checks enforced (see docs/coding_style.md and build_defs/check_banned_patterns.py):
# clang-format 18 including required braces, no `long long`, no `std::aligned_storage`,
# no user-defined literal operators, no hidden Unicode whitespace or typographic
# punctuation, and no new or worsened supported out-of-line method definition
# above the local decision-point complexity limit.
#
# Usage:
#   tools/lint.sh                 # lint project C++ (excluding third_party/)
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

# clang-format 18 is the repository's formatting baseline. A full-tree check keeps
# InsertBraces enforcement independent of Bazel target reachability. Include tracked
# C++ utilities outside donner/ and examples/ while leaving vendored code alone.
format_sources=("${sources[@]}")
if [[ ${lint_explicit_roots} -eq 0 ]]; then
  format_sources=()
  while IFS= read -r -d '' path; do
    if [[ "${path}" != third_party/* && -f "${path}" ]]; then
      format_sources+=("${path}")
    fi
  done < <(git ls-files -z -- '*.cc' '*.h' '*.cpp' '*.hpp' '*.mm')
fi
formatter="${DONNER_CLANG_FORMAT:-clang-format-18}"
if ! command -v "${formatter}" >/dev/null 2>&1; then
  echo "lint.sh: clang-format 18 is required (set DONNER_CLANG_FORMAT to its path)" >&2
  exit 1
fi
"${formatter}" --dry-run --Werror --style="file:${repo_root}/.clang-format" \
  "${format_sources[@]}"

# CodeFactor evaluates complexity only on changed methods. Mirror that locally without making
# existing repository debt block every lint run. The target branch can be overridden for stacked
# work with DONNER_LINT_BASE_REF; GitHub pull-request jobs supply GITHUB_BASE_REF; ordinary local
# branches compare to origin/main. Never use the feature branch's upstream, because an already
# pushed regression would then disappear from the next lint run.
complexity_base_ref="${DONNER_LINT_BASE_REF:-}"
if [[ -z "${complexity_base_ref}" && -n "${GITHUB_BASE_REF:-}" ]]; then
  complexity_base_ref="origin/${GITHUB_BASE_REF}"
fi
if [[ -z "${complexity_base_ref}" ]]; then
  complexity_base_ref="origin/main"
fi
if ! git rev-parse --verify --quiet "${complexity_base_ref}^{commit}" >/dev/null; then
  echo "lint.sh: cannot resolve complexity baseline '${complexity_base_ref}'" >&2
  echo "lint.sh: fetch the target branch or set DONNER_LINT_BASE_REF to its local ref" >&2
  exit 1
fi

complexity_sources=()
if [[ ${lint_explicit_roots} -gt 0 ]]; then
  complexity_sources=("${sources[@]}")
else
  while IFS= read -r path; do
    if [[ -f "${path}" ]]; then
      complexity_sources+=("${path}")
    fi
  done < <(
    {
      git diff --name-only --diff-filter=ACMR "${complexity_base_ref}"...HEAD -- \
        '*.cc' '*.cpp' '*.h' '*.hpp' '*.mm'
      git diff --name-only --diff-filter=ACMR HEAD -- '*.cc' '*.cpp' '*.h' '*.hpp' '*.mm'
      git ls-files --others --exclude-standard -- '*.cc' '*.cpp' '*.h' '*.hpp' '*.mm'
    } | sort -u
  )
fi

if [[ ${#complexity_sources[@]} -gt 0 ]]; then
  python3 "${kChecker}" \
    --check-method-complexity \
    --complexity-baseline-ref "${complexity_base_ref}" \
    "${complexity_sources[@]}"
fi

# Mirror the Python XML and complexity failures before the external review gate.
python_sources=()
while IFS= read -r path; do
  if [[ -f "${path}" ]]; then
    python_sources+=("${path}")
  fi
done < <(
  {
    git diff --name-only --diff-filter=ACMR "${complexity_base_ref}"...HEAD -- '*.py'
    git diff --name-only --diff-filter=ACMR HEAD -- '*.py'
    git ls-files --others --exclude-standard -- '*.py'
  } | sort -u
)
if [[ ${#python_sources[@]} -gt 0 ]]; then
  python3 tools/check_python_quality.py --base-ref "${complexity_base_ref}" "${python_sources[@]}"
fi
