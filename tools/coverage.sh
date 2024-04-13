#!/bin/bash -e

# Print help message
if [[ $1 == "--help" ]]; then
  echo "Usage: $0 [TARGETS...]"
  echo "Run coverage analysis on the specified Bazel targets."
  echo "If no targets are specified, coverage is run on '//src/...'."
  exit 0
fi

# Error if genhtml is not found
if ! which genhtml > /dev/null; then
    echo "ERROR: genhtml not found, please install lcov"
    exit 1
fi

JAVA_HOME=$(dirname $(dirname $(which java)))

TARGETS=${@:-//src/...}

(
  cd $(bazel info workspace)
  bazel coverage $TARGETS

  genhtml $(bazel info output_path)/_coverage/_coverage_report.dat \
    --highlight \
    --legend \
    --branch-coverage \
    --output-directory coverage-report
)

echo "Coverage report saved to coverage-report/index.html"
