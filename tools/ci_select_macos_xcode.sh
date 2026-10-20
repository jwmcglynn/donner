#!/usr/bin/env bash
# Selects the pinned Xcode (tools/ci_macos_xcode.txt) for a macOS CI job, or fails the job.
#
# Usage: ci_select_macos_xcode.sh
#
# Looks the pinned version up the way Bazel's native Xcode locator does, through
# LaunchServices, so the selection does not depend on where a runner installs Xcode. Exactly
# one registered installation may claim the pinned version, and it must report the pinned
# build. The script then exports DEVELOPER_DIR and appends --xcode_version to
# BAZEL_MACOS_BUILD_FLAGS through GITHUB_ENV, and writes cache_partition=<identity> to
# GITHUB_OUTPUT. The identity names only the pinned, public Xcode, so every runner on it shares
# one remote-cache partition. Any other outcome fails the job: a lane on another Xcode cannot
# share action keys with the others.
#
# Install paths are printed only on GitHub-hosted runners. XCODEBUILD, XCODE_LOCATOR, XCRUN and
# XCODE_PROBE_PYTHON override tools in tests.
set -euo pipefail

if [[ $# -ne 0 ]]; then
  echo "usage: $0" >&2
  exit 2
fi

run_probe() {
  "${XCODE_PROBE_PYTHON:-python3}" -c '
import os
import signal
import sys

signal.alarm(30)
try:
    os.execvp(sys.argv[1], sys.argv[1:])
except OSError as error:
    print(f"Cannot execute {sys.argv[1]}: {error}", file=sys.stderr)
    sys.exit(127)
' "$@"
}

pin_file="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/ci_macos_xcode.txt"
pin="$(grep -v '^[[:space:]]*\(#\|$\)' "${pin_file}")"
if [[ ! "${pin}" =~ ^([0-9]+\.[0-9]+(\.[0-9]+)?)\ ([0-9]+[A-Z][0-9]+[a-z]?)$ ]]; then
  echo "::error::${pin_file} must hold one '<version> <build>' line."
  exit 1
fi
version="${BASH_REMATCH[1]}"
build="${BASH_REMATCH[3]}"

scratch="$(mktemp -d "${TMPDIR:-/tmp}/donner-xcode-locator.XXXXXX")"
trap 'rm -rf "${scratch}"' EXIT

show_diagnostics() {
  local diagnostic
  for diagnostic in "$@"; do
    if [[ -s "${diagnostic}" ]]; then
      echo "Xcode discovery probe diagnostics (${diagnostic##*/}):" >&2
      cat "${diagnostic}" >&2
    fi
  done
}

# A self-hosted runner's install paths can name its users or volumes, and its logs are public.
where() {
  if [[ "${RUNNER_ENVIRONMENT:-}" == "github-hosted" ]]; then
    echo "$1"
  else
    echo "the discovered installation"
  fi
}

fail() {
  echo "::error::The pinned Xcode ${version} (${build}) $1"
  exit 1
}

locator="${XCODE_LOCATOR:-}"
if [[ -z "${locator}" ]]; then
  locator="${scratch}/xcode-locator"
  sdk_root="$(run_probe "${XCRUN:-/usr/bin/xcrun}" --sdk macosx --show-sdk-path 2>"${scratch}/sdk.log" || true)"
  if [[ -z "${sdk_root}" ]] || ! (
    unset SDKROOT
    run_probe "${XCRUN:-/usr/bin/xcrun}" clang -fobjc-arc -isysroot "${sdk_root}" \
      -framework Foundation -framework CoreServices \
      "$(dirname "${BASH_SOURCE[0]}")/ci_xcode_discoverable.m" -o "${locator}" \
      >"${scratch}/compile.log" 2>&1
  ); then
    show_diagnostics "${scratch}/sdk.log" "${scratch}/compile.log"
    fail "could not be looked up: the Xcode discovery probe could not be built."
  fi
fi

probe_status=0
developers="$(run_probe "${locator}" "${version}" 2>"${scratch}/probe.log")" || probe_status=$?
if [[ "${probe_status}" != 0 ]]; then
  show_diagnostics "${scratch}/probe.log"
  fail "could not be looked up: the Xcode discovery probe failed (exit ${probe_status})."
fi

count=0
developer_dir=""
while IFS= read -r line; do
  if [[ -n "${line}" ]]; then
    count=$((count + 1))
    developer_dir="${line}"
  fi
done <<< "${developers}"
if [[ "${count}" == 0 ]]; then
  fail "is not installed where Bazel can discover it: LaunchServices lists no Xcode ${version}."
fi
if [[ "${count}" != 1 ]]; then
  if [[ "${RUNNER_ENVIRONMENT:-}" == "github-hosted" ]]; then
    echo "Installations that claim Xcode ${version}:" >&2
    echo "${developers}" >&2
  fi
  fail "is ambiguous: ${count} installations claim version ${version}, and Bazel could pick any of them."
fi

reported="$(DEVELOPER_DIR="${developer_dir}" run_probe "${XCODEBUILD:-xcodebuild}" -version \
  2>"${scratch}/xcodebuild.log" || true)"
if [[ "${reported}" != "Xcode ${version}"$'\n'"Build version ${build}" ]]; then
  echo "$(where "${developer_dir}") reports:" >&2
  echo "${reported}" | head -n 5 >&2
  show_diagnostics "${scratch}/xcodebuild.log"
  fail "is not the Xcode that Bazel discovers for version ${version}."
fi

{
  echo "DEVELOPER_DIR=${developer_dir}"
  echo "BAZEL_MACOS_BUILD_FLAGS=${BAZEL_MACOS_BUILD_FLAGS:-} --xcode_version=${version}"
} >> "${GITHUB_ENV}"
echo "cache_partition=macos-${RUNNER_ARCH:-unknown}-xcode-${version}-${build}" >> "${GITHUB_OUTPUT}"
echo "Selected the pinned Xcode ${version} (${build})."
