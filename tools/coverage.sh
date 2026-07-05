#!/bin/bash -e

function print_help() {
  cat <<EOF
Usage: $0 [--quiet] [--no-html] [--coverage-target PERCENT]
          [--branch-target PERCENT] [--output-dir DIR]
          [--codecov-reference-json PATH] [TARGETS...]
Run coverage analysis on the specified Bazel targets.
If no targets are specified, coverage is run on '//donner/...'.

Options:
  --quiet                   Suppress debug output, print periodic progress, and save logs.
  --no-html                 Skip HTML generation and only emit filtered LCOV output.
  --coverage-target PERCENT Print Codecov-style line shortfall to this target (default: 90).
  --branch-target PERCENT   Print branch-hit shortfall to this target (default: 85).
  --output-dir DIR          Directory for filtered LCOV/HTML output (default: coverage-report).
  --codecov-reference-json PATH
                            Use a Codecov commit API JSON file as the processed file/line universe.
  --help                    Show this help message.
EOF
  exit 0
}

function run_quiet_with_progress() {
  local description="$1"
  local log_file="$2"
  shift 2

  local progress_interval="${DONNER_COVERAGE_PROGRESS_INTERVAL_SECONDS:-60}"
  local start_time
  start_time=$(date +%s)

  echo "$description started; detailed log: $log_file"
  "$@" > "$log_file" 2>&1 &
  local command_pid=$!

  (
    while sleep "$progress_interval"; do
      if ! kill -0 "$command_pid" 2> /dev/null; then
        exit 0
      fi

      local now
      now=$(date +%s)
      local elapsed=$((now - start_time))
      echo "$description still running after ${elapsed}s; detailed log: $log_file"
    done
  ) &
  local progress_pid=$!

  local status=0
  wait "$command_pid" || status=$?
  kill "$progress_pid" 2> /dev/null || true
  wait "$progress_pid" 2> /dev/null || true

  local end_time
  end_time=$(date +%s)
  local elapsed=$((end_time - start_time))
  if [[ "$status" -eq 0 ]]; then
    echo "$description completed in ${elapsed}s"
  else
    echo "$description exited with status $status after ${elapsed}s; detailed log: $log_file"
  fi
  return "$status"
}

function bazel_info() {
  if [ "$QUIET" = true ]; then
    "${BAZEL_CMD[@]}" info "$1" 2> /dev/null
  else
    "${BAZEL_CMD[@]}" info "$1"
  fi
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
COVERAGE_TARGET=90
BRANCH_TARGET=85
COVERAGE_OUTPUT_DIR=coverage-report
CODECOV_REFERENCE_JSON=""
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
    --coverage-target)
      if [[ $# -lt 2 ]]; then
        echo "ERROR: --coverage-target requires a percentage value"
        exit 1
      fi
      COVERAGE_TARGET="$2"
      shift 2
      ;;
    --coverage-target=*)
      COVERAGE_TARGET="${1#--coverage-target=}"
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
    --codecov-reference-json)
      if [[ $# -lt 2 ]]; then
        echo "ERROR: --codecov-reference-json requires a file path"
        exit 1
      fi
      CODECOV_REFERENCE_JSON="$2"
      shift 2
      ;;
    --codecov-reference-json=*)
      CODECOV_REFERENCE_JSON="${1#--codecov-reference-json=}"
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
WORKSPACE_ROOT=$(bazel_info workspace)

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

  COVERAGE_REPORT=$(bazel_info output_path)/_coverage/_coverage_report.dat
  rm -rf "$COVERAGE_HTML_DIR"
  mkdir -p "$COVERAGE_HTML_DIR"
  BAZEL_COVERAGE_LOG="$COVERAGE_HTML_DIR/bazel_coverage.log"
  FILTER_COVERAGE_LOG="$COVERAGE_HTML_DIR/filter_coverage.log"
  GENHTML_LOG="$COVERAGE_HTML_DIR/genhtml.log"

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
    run_quiet_with_progress "Bazel coverage" "$BAZEL_COVERAGE_LOG" \
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

  FILTER_ARGS=(--input "$COVERAGE_REPORT" --output "$COVERAGE_HTML_DIR/filtered_report.dat")

  if [ "$QUIET" = true ]; then
    run_quiet_with_progress "LCOV filtering" "$FILTER_COVERAGE_LOG" \
      python3 tools/filter_coverage.py "${FILTER_ARGS[@]}"
  else
    python3 tools/filter_coverage.py --verbose "${FILTER_ARGS[@]}"
  fi
  python3 tools/check_lcov_report.py "$COVERAGE_HTML_DIR/filtered_report.dat"
  METRICS_ARGS=(
    "$COVERAGE_HTML_DIR/filtered_report.dat"
    --coverage-target "$COVERAGE_TARGET"
    --branch-target "$BRANCH_TARGET"
  )
  if [ -n "$CODECOV_REFERENCE_JSON" ]; then
    METRICS_ARGS+=(--codecov-reference-json "$CODECOV_REFERENCE_JSON")
  fi
  python3 tools/lcov_metrics.py "${METRICS_ARGS[@]}"
  phase_mark filter_done

  if [ "$NO_HTML" = false ]; then
    if [ "$QUIET" = true ]; then
      run_quiet_with_progress "Coverage HTML generation" "$GENHTML_LOG" \
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
