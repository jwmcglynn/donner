#!/usr/bin/env bash
#
# clang_format.sh - run the repository's pinned clang-format with the given arguments.
#
# The binary is //tools/format:clang_format, the same one the `bazel test //...` format checks
# use. Paths in the arguments are relative to the current directory, as with clang-format.
# Xcode 27.1's `xcrun clang-format` matched it byte for byte when it was pinned.
#
# Bazel runs as ${DONNER_BAZEL:-bazel}; set DONNER_BAZEL to add startup flags, for example
# DONNER_BAZEL="bazel --output_user_root=/path/to/root".

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
read -r -a bazel <<<"${DONNER_BAZEL:-bazel}"
if [[ ${#bazel[@]} -eq 0 ]]; then
  bazel=(bazel)
fi

# Each step returns explicitly: older bash (macOS /bin/bash 3.2) does not apply errexit inside the
# command substitution that calls this function.
resolve() {
  cd "${repo_root}" || return
  # The binary must exist locally even when a user rc downloads only minimal remote outputs.
  "${bazel[@]}" build --noshow_progress --ui_event_filters=-info --remote_download_outputs=toplevel \
    //tools/format:clang_format >&2 || return
  local paths path base
  # Use the first listed output that exists; cquery prints one path per configured target.
  paths="$("${bazel[@]}" cquery --noshow_progress --ui_event_filters=-info --output=files \
    //tools/format:clang_format 2>/dev/null)" || return
  local roots=("$("${bazel[@]}" info execution_root 2>/dev/null)"
    "$("${bazel[@]}" info output_base 2>/dev/null)")
  while IFS= read -r path; do
    for base in "${roots[@]}"; do
      if [[ -n "${path}" && -x "${base}/${path}" ]]; then
        echo "${base}/${path}"
        return 0
      fi
    done
  done <<<"${paths}"
  echo "clang_format.sh: cannot find the built clang-format (${paths})" >&2
  return 1
}

binary="$(resolve)"
exec "${binary}" "$@"
