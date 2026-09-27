#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 <capture-binary> <source-revision>" >&2
  exit 2
fi

: "${TEST_UNDECLARED_OUTPUTS_DIR:?Bazel did not provide an undeclared-output directory}"
export WGPU_BACKEND=vulkan

exec "$1" "${TEST_UNDECLARED_OUTPUTS_DIR}/reference" "$2" clean
