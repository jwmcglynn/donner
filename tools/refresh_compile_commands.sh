#!/bin/bash -e
cd "$(dirname "$0")/.."

bazel run //:refresh_compile_commands
