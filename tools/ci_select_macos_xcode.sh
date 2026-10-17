#!/usr/bin/env bash
# Selects the pinned Xcode (tools/ci_macos_xcode.txt) for a macOS CI job.
#
# Usage: ci_select_macos_xcode.sh --required|--optional
#
# When the pinned version and build are installed, exports DEVELOPER_DIR and
# appends --xcode_version to BAZEL_MACOS_BUILD_FLAGS through GITHUB_ENV, and
# writes cache_partition=<identity> to GITHUB_OUTPUT. The identity names only
# the pinned, public Xcode, so every runner on it shares one remote-cache
# partition. When it is not installed, --required fails the job and --optional
# writes an empty cache_partition and a warning, leaving the runner's default
# Xcode in place; it never prints which Xcode the runner does have.
#
# XCODE_APPLICATIONS_DIR, XCODEBUILD and XCODE_LSREGISTER override platform tools
# for tests.
set -euo pipefail

mode="${1:-}"
case "${mode}" in
  --required | --optional) ;;
  *)
    echo "usage: $0 --required|--optional" >&2
    exit 2
    ;;
esac

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
if [[ -d "${developer_dir}" ]]; then
  reported="$(DEVELOPER_DIR="${developer_dir}" "${XCODEBUILD:-xcodebuild}" -version 2>/dev/null || true)"
  if [[ "${reported}" == "Xcode ${version}"$'\n'"Build version ${build}" ]]; then
    installed=true
  fi
fi

if [[ "${installed}" != "true" ]]; then
  echo "cache_partition=" >> "${GITHUB_OUTPUT}"
  if [[ "${mode}" == "--required" ]]; then
    echo "::error::The pinned Xcode ${version} (${build}) is not installed at ${developer_dir}."
    exit 1
  fi
  echo "::warning::The pinned Xcode ${version} (${build}) is not installed on this runner." \
    "This job keeps the runner's default Xcode, so its action keys cannot match the" \
    "GitHub-hosted macOS lanes'."
  exit 0
fi

# Bazel discovers installed Xcode bundles through the current user's LaunchServices registry.
registrar="${XCODE_LSREGISTER:-/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister}"
if ! "${registrar}" -f "${developer_dir%/Contents/Developer}"; then
  echo "::error::Could not register the pinned Xcode ${version} (${build}) for Bazel discovery."
  exit 1
fi

{
  echo "DEVELOPER_DIR=${developer_dir}"
  echo "BAZEL_MACOS_BUILD_FLAGS=${BAZEL_MACOS_BUILD_FLAGS:-} --xcode_version=${version}"
} >> "${GITHUB_ENV}"
echo "cache_partition=macos-${RUNNER_ARCH:-unknown}-xcode-${version}-${build}" >> "${GITHUB_OUTPUT}"
echo "Selected the pinned Xcode ${version} (${build})."
