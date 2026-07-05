#!/bin/bash -e

function print_help() {
  echo "Usage: $0 [--quiet] [--no-html] [--branch-target PERCENT] [--output-dir DIR] [TARGETS...]"
  echo "Run coverage analysis on the specified Bazel targets."
  echo "If no targets are specified, coverage is run on '//donner/...'."
  echo ""
  echo "Options:"
  echo "  --quiet                  Suppress debug output."
  echo "  --no-html                Skip HTML generation and only emit filtered LCOV output."
  echo "  --branch-target PERCENT  Print branch-hit shortfall to this target (default: 85)."
  echo "  --output-dir DIR         Directory for filtered LCOV/HTML output (default: coverage-report)."
  echo "  --help                   Show this help message."
  exit 0
}

TARGETS=()
BAZEL_COVERAGE_FLAGS=()
DEFAULT_BAZEL_COVERAGE_FLAGS=()
read -r -a BAZEL_CMD <<< "${DONNER_BAZEL:-bazel}"

if [[ -n "${DONNER_COVERAGE_BAZEL_FLAGS:-}" ]]; then
  read -r -a BAZEL_COVERAGE_FLAGS <<< "$DONNER_COVERAGE_BAZEL_FLAGS"
else
  DEFAULT_BAZEL_COVERAGE_FLAGS=(
    --remote_executor=
    --remote_cache=
    --experimental_remote_downloader=
    --noremote_upload_local_results
  )
fi

# Check for --quiet option
QUIET=false
NO_HTML=false
BRANCH_TARGET=85
COVERAGE_OUTPUT_DIR=coverage-report
while [[ $# -gt 0 ]]; do
  case "$1" in
    --quiet)
      QUIET=true
      shift
      ;;
    --no-html)
      NO_HTML=true
      shift
      ;;
    --branch-target)
      if [[ $# -lt 2 ]]; then
        echo "ERROR: --branch-target requires a percentage value"
        exit 1
      fi
      BRANCH_TARGET="$2"
      shift 2
      ;;
    --branch-target=*)
      BRANCH_TARGET="${1#--branch-target=}"
      shift
      ;;
    --output-dir)
      if [[ $# -lt 2 ]]; then
        echo "ERROR: --output-dir requires a directory"
        exit 1
      fi
      COVERAGE_OUTPUT_DIR="$2"
      shift 2
      ;;
    --output-dir=*)
      COVERAGE_OUTPUT_DIR="${1#--output-dir=}"
      shift
      ;;
    --help)
      print_help
      ;;
    *)
      TARGETS+=("$1")
      shift
      ;;
  esac
done

# Default to //donner/... if no targets are specified
if [[ ${#TARGETS[@]} -eq 0 ]]; then
  TARGETS=(//donner/...)
fi

echo "Analyzing coverage for: ${TARGETS[*]}"

# Error if genhtml is not found (only required when HTML output is enabled).
if [ "$NO_HTML" = false ] && ! which genhtml > /dev/null; then
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
WORKSPACE_ROOT=$("${BAZEL_CMD[@]}" info workspace)

BAZEL_TEST_ENV=()
if [[ "$(uname)" == "Darwin" ]]; then
  BAZEL_TEST_ENV=(--test_env=DYLD_LIBRARY_PATH="$WORKSPACE_ROOT")
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

LLVM_COVERAGE_FLAGS=(
  --features=llvm_coverage_map_format
  # Bazel's test runner copies GCOV into COVERAGE_GCOV_PATH. For LLVM lcov
  # collection that path is used as llvm-profdata to merge profraw files.
  --action_env=GCOV="$LLVM_PROFDATA_PATH"
  --action_env=BAZEL_LLVM_COV="$LLVM_COV_PATH"
  --action_env=BAZEL_LLVM_PROFDATA="$LLVM_PROFDATA_PATH"
  --test_env=LLVM_COV="$LLVM_COV_PATH"
  --test_env=LLVM_PROFDATA="$LLVM_PROFDATA_PATH"
)

(
  cd "$WORKSPACE_ROOT"

  COVERAGE_HTML_DIR="$COVERAGE_OUTPUT_DIR"
  GENHTML_OPTIONS="--legend --branch-coverage --ignore-errors category --ignore-errors inconsistent --output-directory $COVERAGE_HTML_DIR"

  COVERAGE_REPORT=$("${BAZEL_CMD[@]}" info output_path)/_coverage/_coverage_report.dat

  # CI diagnostics: when DONNER_CI_DIAGNOSTICS_DIR is set (self-hosted CI),
  # capture the inner `bazel coverage` profile + BEP there and record phase
  # wall times, so slow coverage runs are attributable from artifacts alone.
  DIAG_FLAGS=()
  if [ -n "${DONNER_CI_DIAGNOSTICS_DIR:-}" ]; then
    mkdir -p "$DONNER_CI_DIAGNOSTICS_DIR/coverage"
    DIAG_FLAGS=(
      --profile="$DONNER_CI_DIAGNOSTICS_DIR/coverage/profile.gz"
      --build_event_json_file="$DONNER_CI_DIAGNOSTICS_DIR/coverage/bep.json"
    )
  fi
  phase_mark() {
    if [ -n "${DONNER_CI_DIAGNOSTICS_DIR:-}" ]; then
      echo "$1=$(date +%s)" >> "$DONNER_CI_DIAGNOSTICS_DIR/coverage/timing.txt"
    fi
  }
  phase_mark start

  # --keep_going is set in .bazelrc for coverage so that analysis failures
  # don't block the rest of the run. Remove any previous combined report so
  # early exits cannot accidentally process stale data.
  rm -f "$COVERAGE_REPORT"

  if [ "$QUIET" = true ]; then
    "${BAZEL_CMD[@]}" coverage --config=latest_llvm --ui_event_filters=-info,-stdout,-stderr --noshow_progress \
      "${DEFAULT_BAZEL_COVERAGE_FLAGS[@]}" \
      "${BAZEL_COVERAGE_FLAGS[@]}" \
      "${LLVM_COVERAGE_FLAGS[@]}" \
      "${DIAG_FLAGS[@]}" \
      "${BAZEL_TEST_ENV[@]}" "${TARGETS[@]}" || true
  else
    "${BAZEL_CMD[@]}" coverage --config=latest_llvm \
      "${DEFAULT_BAZEL_COVERAGE_FLAGS[@]}" \
      "${BAZEL_COVERAGE_FLAGS[@]}" \
      "${LLVM_COVERAGE_FLAGS[@]}" \
      "${DIAG_FLAGS[@]}" \
      "${BAZEL_TEST_ENV[@]}" "${TARGETS[@]}" || true
  fi
  phase_mark bazel_coverage_done

  if [ ! -f "$COVERAGE_REPORT" ]; then
    echo "ERROR: Coverage report was not generated"
    exit 1
  fi

  rm -rf "$COVERAGE_HTML_DIR"
  mkdir -p "$COVERAGE_HTML_DIR"
  FILTER_ARGS=(--input "$COVERAGE_REPORT" --output "$COVERAGE_HTML_DIR/filtered_report.dat")

  if [ "$QUIET" = true ]; then
    python3 tools/filter_coverage.py "${FILTER_ARGS[@]}"
  else
    python3 tools/filter_coverage.py --verbose "${FILTER_ARGS[@]}"
  fi
  python3 tools/check_lcov_report.py "$COVERAGE_HTML_DIR/filtered_report.dat"
  python3 tools/lcov_metrics.py "$COVERAGE_HTML_DIR/filtered_report.dat" \
    --branch-target "$BRANCH_TARGET"
  phase_mark filter_done

  if [ "$NO_HTML" = false ]; then
    if [ "$QUIET" = true ]; then
      genhtml --quiet "$COVERAGE_HTML_DIR/filtered_report.dat" $GENHTML_OPTIONS
    else
      genhtml "$COVERAGE_HTML_DIR/filtered_report.dat" $GENHTML_OPTIONS
    fi
  fi
  phase_mark end
)

if [ "$NO_HTML" = true ]; then
  echo "Filtered coverage report saved to $COVERAGE_OUTPUT_DIR/filtered_report.dat"
else
  echo "Coverage report saved to $COVERAGE_OUTPUT_DIR/index.html"
fi
