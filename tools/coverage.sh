#!/bin/bash -e
cd "$(dirname "$0")/.."

JAVA_HOME=$(dirname $(dirname $(which java)))

bazel coverage -s \
    --instrument_test_targets \
    --experimental_cc_coverage \
    --instrumentation_filter="//src[:/]" \
    --combined_report=lcov \
    --local_test_jobs=1 \
    --nocache_test_results \
    //src/...

genhtml bazel-out/_coverage/_coverage_report.dat \
    --highlight \
    --legend \
    --output-directory coverage-report
