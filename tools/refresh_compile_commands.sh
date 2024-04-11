#!/bin/bash -e
cd "$(dirname "$0")/.."

bazel run //tools:refresh_compile_commands
