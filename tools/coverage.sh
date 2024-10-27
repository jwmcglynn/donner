#!/bin/bash -e

function print_help() {
  echo "Usage: $0 [--quiet] [TARGETS...]"
  echo "Run coverage analysis on the specified Bazel targets."
  echo "If no targets are specified, coverage is run on '//donner/...'."
  echo ""
  echo "Options:"
  echo "  --quiet  Suppress debug output."
  echo "  --help   Show this help message."
  exit 0
}

TARGETS=()

# Check for --quiet option
QUIET=false
for arg in "$@"; do
  # --quiet: suppress debug output
  if [[ $arg == "--quiet" ]]; then
    QUIET=true
  elif [[ $arg == "--help" ]]; then
    print_help
  else
    TARGETS+=("$arg")
  fi
done

# Default to //donner/... if no targets are specified
TARGETS=${TARGETS:-//donner/...}

echo "Analyzing coverage for: $TARGETS"

# Error if genhtml is not found
if ! which genhtml > /dev/null; then
    echo "ERROR: genhtml not found, please install lcov"
    exit 1
fi

# Error if java is not found
if ! which java > /dev/null; then
    echo "ERROR: Java not found. Please install a Java runtime environment (JRE)"
    echo "On Ubuntu/Debian: sudo apt install default-jre"
    echo "On macOS: brew install java"
    exit 1
fi

JAVA_HOME=$(dirname $(dirname $(which java)))

(
  cd $(bazel info workspace)

  GENHTML_OPTIONS="--highlight --legend --branch-coverage --ignore-errors category --output-directory coverage-report"

  if [ "$QUIET" = true ]; then
    bazel coverage --config=latest_llvm --ui_event_filters=-info,-stdout,-stderr --noshow_progress $TARGETS
    genhtml --quiet $(bazel info output_path)/_coverage/_coverage_report.dat $GENHTML_OPTIONS
  else
    bazel coverage --config=latest_llvm $TARGETS
    genhtml $(bazel info output_path)/_coverage/_coverage_report.dat $GENHTML_OPTIONS
  fi
)

echo "Coverage report saved to coverage-report/index.html"
