#!/usr/bin/env bash
#
# cmake_consumer.sh - build and test examples/cmake_consumer against the generated CMake build.
#
# This is the CMake lane's single entry point, so CI and a contributor run the same commands:
#
#   tools/ci/cmake_consumer.sh install-tools
#       Install ccache and ninja with Homebrew when either is missing (hosted macOS images).
#   tools/ci/cmake_consumer.sh build [--check] [--ccache] [--release-bazel-server] [--parallel N]
#       --check     first validate the checked-in generator output (gen_cmakelists.py --check)
#       --ccache    compile through ccache, capped at $CCACHE_MAXSIZE, and report its statistics
#       --release-bazel-server  stop this lane's query/build server before native compilation
#       --parallel  build with N jobs; without it CMake's own default applies
#   Main CI uses --check --release-bazel-server --parallel 4.
#
# Each phase is a collapsible `::group::` in the GitHub Actions log, and when
# $GITHUB_STEP_SUMMARY is set the phases and their results are appended to the job summary.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
cd "${script_dir}/../.."

usage() {
  sed -n '3,16p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2
  exit 2
}

# shellcheck source=tools/ci/lane.sh
source "tools/ci/lane.sh"

install_tools() {
  local missing=()
  # Homebrew is the hosted macOS image's package manager; Linux runners use the apt-install action.
  command -v ccache >/dev/null || missing+=(ccache)
  command -v ninja >/dev/null || missing+=(ninja)
  if [[ ${#missing[@]} -gt 0 ]]; then
    brew install "${missing[@]}"
  fi
}

build() {
  local check=0 use_ccache=0 release_bazel_server=0
  local parallel=(--parallel)
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --check) check=1 ;;
      --ccache) use_ccache=1 ;;
      --release-bazel-server) release_bazel_server=1 ;;
      --parallel)
        [[ $# -ge 2 ]] || usage
        parallel=(--parallel "$2")
        shift
        ;;
      *) usage ;;
    esac
    shift
  done
  if [[ ${use_ccache} -eq 1 && -z "${CCACHE_MAXSIZE:-}" ]]; then
    echo "cmake_consumer.sh: --ccache needs CCACHE_MAXSIZE" >&2
    exit 2
  fi

  local configure=(cmake -S examples/cmake_consumer -B build -G Ninja
    -DDONNER_SOURCE_DIR="${PWD}"
    -DDONNER_RENDERER_BACKEND=tiny_skia)
  if [[ ${use_ccache} -eq 1 ]]; then
    configure+=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
  fi

  lane_begin "CMake consumer (tiny_skia)"
  if [[ ${use_ccache} -eq 1 ]]; then
    lane_phase "Reset ccache statistics and size cap" ccache_reset
  fi
  if [[ ${check} -eq 1 ]]; then
    lane_phase "Validate generated CMakeLists.txt" python3 tools/cmake/gen_cmakelists.py --check
  fi
  lane_phase "Generate CMakeLists.txt" python3 tools/cmake/gen_cmakelists.py
  if [[ ${release_bazel_server} -eq 1 ]]; then
    lane_phase "Release Bazel server memory" bazel shutdown
  fi
  lane_phase "Configure" "${configure[@]}"
  lane_phase "Build donner_cmake_consumer" \
    cmake --build build --target donner_cmake_consumer "${parallel[@]}"
  lane_phase "Test" ctest --test-dir build --output-on-failure
  if [[ ${use_ccache} -eq 1 ]]; then
    lane_phase "ccache statistics" ccache --show-stats
  fi
}

# Chained with && because lane_phase runs its command with errexit suspended.
ccache_reset() {
  ccache --zero-stats && ccache --max-size="${CCACHE_MAXSIZE}"
}

[[ $# -ge 1 ]] || usage
command="$1"
shift
case "${command}" in
  install-tools)
    [[ $# -eq 0 ]] || usage
    install_tools
    ;;
  build) build "$@" ;;
  *) usage ;;
esac
