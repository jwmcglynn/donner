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

$DOXYGEN_BIN Doxyfile

echo "Documentation generated in generated-doxygen/html/index.html"
