#!/usr/bin/env bash
#
# free_hosted_macos_disk.sh - delete the Xcodes and simulator runtimes a GitHub-hosted macOS image
# carries beside the selected Xcode, so a cold build of the tree fits on its disk. The xcode-27
# image starts a job with about a third of the free space macos-26 did.
#
# Usage: tools/ci/free_hosted_macos_disk.sh
#
# Run after tools/ci_select_macos_xcode.sh, which exports DEVELOPER_DIR. Refuses to run anywhere
# but a disposable GitHub-hosted runner. XCODE_APPLICATIONS_DIR and LSREGISTER override the
# Applications directory and LaunchServices tool in tests.

set -euo pipefail

if [[ $# -ne 0 ]]; then
  echo "usage: tools/ci/free_hosted_macos_disk.sh" >&2
  exit 2
fi
if [[ "${RUNNER_ENVIRONMENT:-}" != "github-hosted" || "${RUNNER_OS:-}" != "macOS" ]]; then
  echo "::error::free_hosted_macos_disk.sh deletes installed Xcodes; it runs only on a GitHub-hosted macOS runner."
  exit 1
fi
if [[ -z "${DEVELOPER_DIR:-}" || ! -d "${DEVELOPER_DIR}" ]]; then
  echo "::error::free_hosted_macos_disk.sh needs the selected Xcode's DEVELOPER_DIR."
  exit 1
fi

lsregister="${LSREGISTER:-/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister}"
keep="$(cd "${DEVELOPER_DIR}/../.." && pwd -P)"
if [[ "${keep}" != *.app || ! "${DEVELOPER_DIR}" -ef "${keep}/Contents/Developer" ]]; then
  echo "::error::free_hosted_macos_disk.sh needs DEVELOPER_DIR inside an Xcode bundle."
  exit 1
fi

available_kib() {
  df -Pk / | awk 'NR == 2 { print $4 }'
}

echo "::group::Disk before cleanup"
df -h /
echo "::endgroup::"
before="$(available_kib)"

# Resolve every entry first: deleting a bundle leaves its symlinks dangling.
bundles=()
links=()
for app in "${XCODE_APPLICATIONS_DIR:-/Applications}"/Xcode*.app; do
  # Skips an unmatched glob and a symlink that already dangles.
  [[ -e "${app}" ]] || continue
  if [[ "$(cd "${app}" && pwd -P)" == "${keep}" ]]; then
    continue
  fi
  if [[ -L "${app}" ]]; then
    links+=("${app}")
  else
    bundles+=("${app}")
  fi
done

# Bazel's Xcode locator fails on a registered bundle it cannot open, so unregister before deleting.
for app in ${bundles[@]+"${bundles[@]}"}; do
  echo "Removing ${app}"
  "${lsregister}" -u "${app}"
done
if ((${#bundles[@]} + ${#links[@]})); then
  sudo rm -rf ${bundles[@]+"${bundles[@]}"} ${links[@]+"${links[@]}"}
fi
# The image's default Xcode may be gone; keep xcode-select on one that exists.
sudo xcode-select --switch "${DEVELOPER_DIR}"

# The disk check that follows enforces the space a build needs, so a runtime that cannot be
# deleted is reported rather than fatal.
if ! xcrun simctl runtime delete all; then
  echo "::warning::Could not delete the image's simulator runtimes."
fi

echo "::group::Disk after cleanup"
df -h /
echo "::endgroup::"
after="$(available_kib)"
echo "Freed $(((after - before) / 1048576)) GiB: $((before / 1048576)) GiB free before, $((after / 1048576)) GiB after."
