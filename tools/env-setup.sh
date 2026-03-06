#!/usr/bin/env bash
# Environment setup for tiny-skia-cpp.
# Installs Bazelisk as a drop-in `bazel` replacement.
#
# Usage:
#   ./tools/env-setup.sh            # install to /usr/local/bin (may need sudo)
#   INSTALL_DIR=~/.local/bin ./tools/env-setup.sh  # install to a user directory

set -euo pipefail

INSTALL_DIR="${INSTALL_DIR:-/usr/local/bin}"
BAZELISK_VERSION="${BAZELISK_VERSION:-v1.25.0}"

os="$(uname -s | tr '[:upper:]' '[:lower:]')"
arch="$(uname -m)"
case "${arch}" in
  x86_64)  arch="amd64" ;;
  aarch64) arch="arm64" ;;
esac

url="https://github.com/bazelbuild/bazelisk/releases/download/${BAZELISK_VERSION}/bazelisk-${os}-${arch}"
dest="${INSTALL_DIR}/bazel"

echo "Installing Bazelisk ${BAZELISK_VERSION} (${os}-${arch}) to ${dest}"

if command -v bazel &>/dev/null; then
  echo "bazel already on PATH: $(command -v bazel)"
  bazel --version
  exit 0
fi

mkdir -p "${INSTALL_DIR}"
curl -fSL -o "${dest}" "${url}"
chmod +x "${dest}"

echo "Installed successfully:"
"${dest}" --version
