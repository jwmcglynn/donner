#!/usr/bin/env bash
# Selects the pinned Xcode (tools/ci_macos_xcode.txt) for a macOS CI job.
#
# Usage: ci_select_macos_xcode.sh --required|--optional
#
# When the pinned version and build are installed and discoverable by Bazel, exports DEVELOPER_DIR and
# appends --xcode_version to BAZEL_MACOS_BUILD_FLAGS through GITHUB_ENV, and
# writes cache_partition=<identity> to GITHUB_OUTPUT. The identity names only
# the pinned, public Xcode, so every runner on it shares one remote-cache
# partition. When it is unavailable, --required fails the job and --optional
# writes an empty cache_partition and a warning, leaving the runner's default
# Xcode in place; it never prints which Xcode the runner does have.
#
# XCODE_APPLICATIONS_DIR and XCODEBUILD override /Applications and xcodebuild
# for tests. XCODE_LOCATOR, XCRUN and XCODE_PROBE_PYTHON override probe tools in tests.
set -euo pipefail

mode="${1:-}"
case "${mode}" in
  --required | --optional) ;;
  *)
    echo "usage: $0 --required|--optional" >&2
    exit 2
    ;;
esac

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

developer_dir="${XCODE_APPLICATIONS_DIR:-/Applications}/Xcode_${version}.app/Contents/Developer"
installed=false
unavailable_reason="is not installed at ${developer_dir}"
if [[ -d "${developer_dir}" ]]; then
  reported="$(DEVELOPER_DIR="${developer_dir}" "${XCODEBUILD:-xcodebuild}" -version 2>/dev/null || true)"
  if [[ "${reported}" == "Xcode ${version}"$'\n'"Build version ${build}" ]]; then
    installed=true
  fi
fi

# Bazel's native Xcode locator uses LaunchServices, which can omit a valid bundle.
# Do not select a toolchain that its actions cannot locate. Optional lanes already
# permit the default Xcode with shared caching disabled when the pin is unusable.
if [[ "${installed}" == "true" ]]; then
  locator="${XCODE_LOCATOR:-}"
  if [[ -z "${locator}" ]]; then
    scratch="$(mktemp -d "${TMPDIR:-/tmp}/donner-xcode-locator.XXXXXX")"
    trap 'rm -rf "${scratch}"' EXIT
    locator="${scratch}/xcode-locator"
    sdk_root="$(DEVELOPER_DIR="${developer_dir}" run_probe "${XCRUN:-/usr/bin/xcrun}" --sdk macosx --show-sdk-path 2>"${scratch}/sdk.log" || true)"
    if [[ -z "${sdk_root}" ]] || ! (
      unset SDKROOT
      export DEVELOPER_DIR="${developer_dir}"
      run_probe "${XCRUN:-/usr/bin/xcrun}" clang -fobjc-arc -isysroot "${sdk_root}" \
        -framework Foundation -framework CoreServices \
        "$(dirname "${BASH_SOURCE[0]}")/ci_xcode_discoverable.m" -o "${locator}" \
        >"${scratch}/compile.log" 2>&1
    ); then
      for diagnostic in "${scratch}/sdk.log" "${scratch}/compile.log"; do
        if [[ -s "${diagnostic}" ]]; then
          echo "Xcode discovery probe diagnostics (${diagnostic##*/}):" >&2
          cat "${diagnostic}" >&2
        fi
      done
      installed=false
      unavailable_reason="could not be checked because the Xcode discovery probe could not be built"
    fi
  fi
  if [[ "${installed}" == "true" ]]; then
    locator_status=0
    if [[ -n "${XCODE_LOCATOR:-}" ]]; then
      "${locator}" "${version}" "${developer_dir}" >/dev/null 2>&1 || locator_status=$?
    else
      run_probe "${locator}" "${version}" "${developer_dir}" \
        >/dev/null 2>&1 || locator_status=$?
    fi
    if [[ "${locator_status}" != 0 ]]; then
      installed=false
      if [[ "${locator_status}" == 1 ]]; then
        unavailable_reason="cannot be discovered unambiguously by Bazel"
      else
        unavailable_reason="could not be checked because the Xcode discovery probe failed"
      fi
    fi
  fi
fi

if [[ "${installed}" != "true" ]]; then
  echo "cache_partition=" >> "${GITHUB_OUTPUT}"
  if [[ "${mode}" == "--required" ]]; then
    echo "::error::The pinned Xcode ${version} (${build}) ${unavailable_reason}."
    exit 1
  fi
  echo "::warning::The pinned Xcode ${version} (${build}) ${unavailable_reason}." \
    "This job keeps the runner's default Xcode, so its action keys cannot match the" \
    "GitHub-hosted macOS lanes'."
  exit 0
fi

{
  echo "DEVELOPER_DIR=${developer_dir}"
  echo "BAZEL_MACOS_BUILD_FLAGS=${BAZEL_MACOS_BUILD_FLAGS:-} --xcode_version=${version}"
} >> "${GITHUB_ENV}"
echo "cache_partition=macos-${RUNNER_ARCH:-unknown}-xcode-${version}-${build}" >> "${GITHUB_OUTPUT}"
echo "Selected the pinned Xcode ${version} (${build})."
