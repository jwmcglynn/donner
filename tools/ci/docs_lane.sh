#!/usr/bin/env bash
#
# docs_lane.sh - the documentation build steps of the docs workflow (build_docs.yaml).
#
#   tools/ci/docs_lane.sh install-doxygen
#       Download the pinned Doxygen release, verify its SHA-256 before extracting it under
#       $GITHUB_WORKSPACE/doxygen (the repository root outside Actions), and put its bin
#       directory on $GITHUB_PATH, or print it when $GITHUB_PATH is unset.
#   tools/ci/docs_lane.sh record-dependencies
#       Resolve the production module graph of the documented `donner` module into
#       MODULE.bazel.lock, prove the lockfile complete in error mode, and record the docs build
#       provenance for $RUNNER_TEMP/artifact.tar in $RUNNER_TEMP/docs-provenance.
#
# Each record-dependencies phase is a `::group::` in the Actions log and a row in the job summary;
# the first failing phase ends the lane with its exit status.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
cd "${script_dir}/../.."

usage() {
  sed -n '3,15p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2
  exit 2
}

# shellcheck source=tools/ci/lane.sh
source "tools/ci/lane.sh"

# The Doxygen release the site is generated with. tools/pages_artifact.py records the version the
# binary reports in the docs build provenance.
readonly DOXYGEN_VERSION="1.15.0"
readonly DOXYGEN_URL="https://github.com/doxygen/doxygen/releases/download/Release_1_15_0/doxygen-1.15.0.linux.bin.tar.gz"
readonly DOXYGEN_SHA256="0ec2e5b2c3cd82b7106d19cb42d8466450730b8cb7a9e85af712be38bf4523a1"

install_doxygen() {
  local workspace="${GITHUB_WORKSPACE:-${PWD}}"
  local directory="${workspace}/doxygen"
  local archive="${directory}/doxygen.tar.gz"
  mkdir -p "${directory}"
  curl -fL "${DOXYGEN_URL}" -o "${archive}"
  # The digest is checked before the archive is extracted.
  echo "${DOXYGEN_SHA256}  ${archive}" | sha256sum --check --strict
  tar xzf "${archive}" -C "${directory}"
  local bin="${directory}/doxygen-${DOXYGEN_VERSION}/bin"
  if [[ -n "${GITHUB_PATH:-}" ]]; then
    echo "${bin}" >>"${GITHUB_PATH}"
  else
    echo "Add ${bin} to PATH to use Doxygen ${DOXYGEN_VERSION}."
  fi
}

# The graph itself is not logged: the lockfile it writes is what the provenance retains.
mod_graph() {
  bazelisk mod graph --ignore_dev_dependency "--lockfile_mode=$1" >/dev/null
}

record_dependencies() {
  : "${RUNNER_TEMP:?record-dependencies needs RUNNER_TEMP, the directory holding artifact.tar}"
  lane_begin "Docs build dependency state"
  lane_phase "Resolve the production module graph" mod_graph update
  lane_phase "Prove the generated lockfile complete" mod_graph error
  lane_phase "Record the docs build provenance" \
    python3 -B -m tools.pages_artifact provenance \
    --archive "${RUNNER_TEMP}/artifact.tar" --lockfile MODULE.bazel.lock \
    --output "${RUNNER_TEMP}/docs-provenance"
}

[[ $# -eq 1 ]] || usage
case "$1" in
  install-doxygen) install_doxygen ;;
  record-dependencies) record_dependencies ;;
  *) usage ;;
esac
