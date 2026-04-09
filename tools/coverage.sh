#!/bin/bash -e

function print_help() {
  echo "Usage: $0 [--quiet] [--no-html] [TARGETS...]"
  echo "Run coverage analysis on the specified Bazel targets."
  echo "If no targets are specified, coverage is run on '//donner/...'."
  echo ""
  echo "Options:"
  echo "  --quiet  Suppress debug output."
  echo "  --no-html  Skip HTML generation and only emit filtered LCOV output."
  echo "  --help   Show this help message."
  exit 0
}

TARGETS=()

# Check for --quiet option
QUIET=false
NO_HTML=false
for arg in "$@"; do
  # --quiet: suppress debug output
  if [[ $arg == "--quiet" ]]; then
    QUIET=true
  elif [[ $arg == "--no-html" ]]; then
    NO_HTML=true
  elif [[ $arg == "--help" ]]; then
    print_help
  else
    TARGETS+=("$arg")
  fi
done

# Default to //donner/... if no targets are specified
if [[ ${#TARGETS[@]} -eq 0 ]]; then
  TARGETS=(//donner/...)
fi

echo "Analyzing coverage for: ${TARGETS[*]}"

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

BAZEL_TEST_ENV=""
if [[ "$(uname)" == "Darwin" ]]; then
  BAZEL_TEST_ENV="--test_env=DYLD_LIBRARY_PATH=$(bazel info workspace)"
fi

LLVM_COV_PATH=""
LLVM_PROFDATA_PATH=""
if command -v clang >/dev/null 2>&1; then
  LLVM_COV_PATH=$(clang --print-prog-name=llvm-cov 2>/dev/null || true)
  LLVM_PROFDATA_PATH=$(clang --print-prog-name=llvm-profdata 2>/dev/null || true)
fi

if [[ -z "$LLVM_COV_PATH" || ! -x "$LLVM_COV_PATH" ]]; then
  echo "ERROR: llvm-cov not found. Install LLVM or ensure clang can locate llvm-cov."
  exit 1
fi

if [[ -z "$LLVM_PROFDATA_PATH" || ! -x "$LLVM_PROFDATA_PATH" ]]; then
  echo "ERROR: llvm-profdata not found. Install LLVM or ensure clang can locate llvm-profdata."
  exit 1
fi

(
  cd $(bazel info workspace)

  GENHTML_OPTIONS="--legend --branch-coverage --ignore-errors category --ignore-errors inconsistent --output-directory coverage-report"

  COVERAGE_REPORT=$(bazel info output_path)/_coverage/_coverage_report.dat

  # --keep_going is set in .bazelrc for coverage so that analysis failures
  # (e.g. Skia ObjC on macOS with LLVM toolchain) don't block the rest of the
  # run.  Record the timestamp before running so we can verify a fresh report
  # was produced (avoid processing stale data on early exit).
  BEFORE_TS=0
  if [ -f "$COVERAGE_REPORT" ]; then
    BEFORE_TS=$(stat -f %m "$COVERAGE_REPORT" 2>/dev/null || stat -c %Y "$COVERAGE_REPORT" 2>/dev/null || echo 0)
  fi

  if [ "$QUIET" = true ]; then
    bazel coverage --config=latest_llvm --ui_event_filters=-info,-stdout,-stderr --noshow_progress \
      --action_env=BAZEL_LLVM_COV=$LLVM_COV_PATH --action_env=GCOV=$LLVM_PROFDATA_PATH \
      $BAZEL_TEST_ENV "${TARGETS[@]}" || true
  else
    bazel coverage --config=latest_llvm --action_env=BAZEL_LLVM_COV=$LLVM_COV_PATH \
      --action_env=GCOV=$LLVM_PROFDATA_PATH $BAZEL_TEST_ENV "${TARGETS[@]}" || true
  fi

  if [ ! -f "$COVERAGE_REPORT" ]; then
    echo "ERROR: Coverage report was not generated"
    exit 1
  fi

  AFTER_TS=$(stat -f %m "$COVERAGE_REPORT" 2>/dev/null || stat -c %Y "$COVERAGE_REPORT" 2>/dev/null || echo 0)
  if [ "$AFTER_TS" -le "$BEFORE_TS" ]; then
    echo "ERROR: Coverage report was not updated (stale data from a previous run)"
    exit 1
  fi

  mkdir -p coverage-report
  FILTER_ARGS=(--input "$COVERAGE_REPORT" --output coverage-report/filtered_report.dat)

  if [ "$QUIET" = true ]; then
    python3 tools/filter_coverage.py "${FILTER_ARGS[@]}"
  else
    python3 tools/filter_coverage.py --verbose "${FILTER_ARGS[@]}"
  fi

  if [ "$NO_HTML" = false ]; then
    if [ "$QUIET" = true ]; then
      genhtml --quiet coverage-report/filtered_report.dat $GENHTML_OPTIONS
    else
      genhtml coverage-report/filtered_report.dat $GENHTML_OPTIONS
    fi
  fi
)

if [ "$NO_HTML" = true ]; then
  echo "Filtered coverage report saved to coverage-report/filtered_report.dat"
else
  echo "Coverage report saved to coverage-report/index.html"
fi
