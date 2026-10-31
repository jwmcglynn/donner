#!/usr/bin/env bash
# Selects the first installed approved Xcode from tools/ci_macos_xcode.txt.
#
# Usage: ci_select_macos_xcode.sh
#
# Discovery follows Bazel's LaunchServices lookup. Only absence permits trying
# the next tuple; ambiguity, discovery failure or a build mismatch fails closed.
# The actual version/build binds the remote partition and all Bazel action/test
# keys, including disk caches. Equal selected tuples retain cross-runner parity.
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
pins=()
versions=" "
while IFS= read -r pin || [[ -n "${pin}" ]]; do
  [[ "${pin}" =~ ^[[:space:]]*(#.*)?$ ]] && continue
  if [[ ${#pins[@]} -ge 2 ||
        ! "${pin}" =~ ^([0-9]+\.[0-9]+(\.[0-9]+)?)\ ([0-9]+[A-Z][0-9]+[a-z]?)$ ]]; then
    echo "::error::The Xcode policy must hold one or two '<version> <build>' lines."
    exit 1
  fi
  version="${BASH_REMATCH[1]}"
  if [[ "${versions}" == *" ${version} "* ]]; then
    echo "::error::The Xcode policy must name distinct versions."
    exit 1
  fi
  versions+="${version} "
  pins+=("${pin}")
done < "${pin_file}"
if [[ ${#pins[@]} -eq 0 ]]; then
  echo "::error::The Xcode policy is empty."
  exit 1
fi
read -r version build <<< "${pins[0]}"

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

selected=false
for pin in "${pins[@]}"; do
  read -r version build <<< "${pin}"
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
    continue
  fi
  if [[ "${count}" != 1 ]]; then
    if [[ "${RUNNER_ENVIRONMENT:-}" == "github-hosted" ]]; then
      echo "Installations that claim Xcode ${version}:" >&2
      echo "${developers}" >&2
    fi
    fail "is ambiguous: ${count} installations claim version ${version}, and Bazel could pick any of them."
  fi

  report_status=0
  reported="$(DEVELOPER_DIR="${developer_dir}" run_probe "${XCODEBUILD:-xcodebuild}" -version \
    2>"${scratch}/xcodebuild.log")" || report_status=$?
  if [[ "${report_status}" != 0 ]]; then
    show_diagnostics "${scratch}/xcodebuild.log"
    fail "could not report its version: xcodebuild failed (exit ${report_status})."
  fi
  if [[ "${reported}" != "Xcode ${version}"$'\n'"Build version ${build}" ]]; then
    echo "$(where "${developer_dir}") reports:" >&2
    echo "${reported}" | head -n 5 >&2
    show_diagnostics "${scratch}/xcodebuild.log"
    fail "is not the Xcode that Bazel discovers for version ${version}."
  fi

  selected=true
  break
done
if [[ "${selected}" != true ]]; then
  fail "is unavailable: LaunchServices lists no Xcode from the approved policy."
fi

{
  echo "DEVELOPER_DIR=${developer_dir}"
  echo "DONNER_CI_XCODE_IDENTITY=${version}-${build}"
  echo "CCACHE_NAMESPACE=donner-xcode-${version}-${build}"
  echo "BAZEL_MACOS_BUILD_FLAGS=${BAZEL_MACOS_BUILD_FLAGS:-} --xcode_version=${version}"
} >> "${GITHUB_ENV}"
echo "cache_partition=macos-${RUNNER_ARCH:-unknown}-xcode-${version}-${build}" >> "${GITHUB_OUTPUT}"
echo "Selected the approved Xcode ${version} (${build})."
