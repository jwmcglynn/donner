#!/bin/bash -e
cd "${0%/*}"

echo "Generating compile commands..."
bazel run //tools:sourcegraph_compile_commands

echo "Building sourcegraph index, this may take a while..."
bazel run //third_party/scip-clang -- --compdb-path compile_commands.json

echo "Done!"
