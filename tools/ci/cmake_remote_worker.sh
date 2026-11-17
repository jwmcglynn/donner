#!/usr/bin/env bash
# The coordinator stages source inputs. Configure, compile, link and run the consumer here.
set -euo pipefail
expected="$1"
[[ "$(uname -s)" == "$expected" ]] || { echo "CMake worker OS mismatch, expected $expected" >&2; exit 1; }
if [[ "$expected" == Darwin ]]; then
  export PATH="$PATH:/opt/homebrew/bin"
fi
cd "$TEST_TMPDIR"
tar -xf "$TEST_SRCDIR/$2"
cmake -S src/examples/cmake_consumer -B build -G Ninja \
  -DDONNER_SOURCE_DIR="$PWD/src" -DDONNER_RENDERER_BACKEND=tiny_skia \
  -DFETCHCONTENT_BASE_DIR="$PWD/deps" -DFETCHCONTENT_FULLY_DISCONNECTED=ON
cmake --build build --target donner_cmake_consumer --parallel 4
ctest --test-dir build --output-on-failure --no-tests=error \
  --output-junit "$TEST_UNDECLARED_OUTPUTS_DIR/consumer.xml"
