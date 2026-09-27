#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 <capture-binary> <source-revision>" >&2
  exit 2
fi

: "${TEST_UNDECLARED_OUTPUTS_DIR:?Bazel did not provide an undeclared-output directory}"
: "${VK_ICD_FILENAMES:?Bazel did not select a Vulkan ICD}"

host_os="$(uname -s)"
host_arch="$(uname -m)"
printf 'capture execution platform: %s %s\n' "$host_os" "$host_arch"
if [[ "$host_os" != "Linux" || "$host_arch" != "aarch64" ]]; then
  echo "capture requires Linux aarch64, got ${host_os} ${host_arch}" >&2
  exit 1
fi
if [[ ! -r "$VK_ICD_FILENAMES" ]]; then
  echo "configured Vulkan ICD is not readable: ${VK_ICD_FILENAMES}" >&2
  exit 1
fi

export WGPU_BACKEND=vulkan

exec "$1" "${TEST_UNDECLARED_OUTPUTS_DIR}/reference" "$2" clean
