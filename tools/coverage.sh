#!/bin/bash -e
cd "$(dirname "$0")/.."

JAVA_HOME=$(dirname $(dirname $(which java)))

bazel coverage -s \
    --local_test_jobs=1 \
    --nocache_test_results \
    //src/...

# Error if genhtml is not found
if ! which genhtml > /dev/null; then
    echo "ERROR: genhtml not found, please install lcov"
    exit 1
fi

genhtml bazel-out/_coverage/_coverage_report.dat \
    --highlight \
    --legend \
    --output-directory coverage-report

echo "Coverage report saved to coverage-report/index.html"
