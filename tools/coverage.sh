#!/bin/bash -e
cd "$(dirname "$0")/.."

JAVA_HOME=$(dirname $(dirname $(which java)))

bazel coverage -s \
    --local_test_jobs=1 \
    --nocache_test_results \
    //src/css/... //src/svg/...

genhtml bazel-out/_coverage/_coverage_report.dat \
    --highlight \
    --legend \
    --output-directory coverage-report
