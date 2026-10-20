#!/usr/bin/env bash
#
# free_runner_disk.sh - delete preinstalled toolchains a hosted Ubuntu runner carries but Donner
# never uses, so a large instrumented build fits on its disk. Only for disposable CI runners.
#
# Usage: tools/ci/free_runner_disk.sh

set -euo pipefail

if [[ $# -ne 0 ]]; then
  echo "usage: tools/ci/free_runner_disk.sh" >&2
  exit 2
fi

echo "::group::Disk before cleanup"
df -h /
echo "::endgroup::"

sudo rm -rf \
  /opt/ghc \
  /opt/hostedtoolcache/CodeQL \
  /usr/local/.ghcup \
  /usr/local/lib/android \
  /usr/local/share/boost \
  /usr/share/dotnet \
  /usr/share/swift
# Docker images are a cache; a failed prune must not fail the job.
docker system prune --all --force || true
sudo apt-get clean

echo "::group::Disk after cleanup"
df -h /
echo "::endgroup::"
