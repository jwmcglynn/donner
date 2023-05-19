#!/bin/bash -e
cd "${0%/*}/.."

bazel query 'attr(testonly, 0, //src/...)' | xargs bazel build --config=doc --output_groups=hdoc
bazel run @hdoc//:hdoc-exporter --run_under="cd $PWD &&" -- --config .hdoc.toml --input $(find bazel-bin/src -name '*.hdoc-payload.json')
