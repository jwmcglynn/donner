#!/bin/bash -e
cd "${0%/*}/.."

# Check if specific doxygen version exists
DOXYGEN_BIN="doxygen"

if [ -d "/workspace/doxygen" ]; then
  LATEST_DOXYGEN=$(find /workspace/doxygen -path '*/bin/doxygen' -type f | sort -rV | head -n1)
  if [ -n "$LATEST_DOXYGEN" ]; then
    DOXYGEN_BIN="$LATEST_DOXYGEN"
    echo "Using specific doxygen version: $DOXYGEN_BIN"
  fi
fi

DONNER_VERSION=$(
  sed -n '/^[[:space:]]*module(/{
    s/.*version[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/
    p
    q
  }' MODULE.bazel
)
if [ -z "$DONNER_VERSION" ]; then
  echo "Unable to read Donner version from MODULE.bazel" >&2
  exit 1
fi
export DONNER_VERSION

$DOXYGEN_BIN Doxyfile

echo "Documentation generated in generated-doxygen/html/index.html"
