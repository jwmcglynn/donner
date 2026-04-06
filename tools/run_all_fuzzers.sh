#!/bin/bash
# Run all 21 fuzzers for 10 minutes each, in parallel batches of 4.
# Results are saved to /tmp/fuzzer_results/

set -euo pipefail

RESULTS_DIR="/tmp/fuzzer_results"
rm -rf "$RESULTS_DIR"
mkdir -p "$RESULTS_DIR"

FUZZERS=(
  "//donner/base/encoding:decompress_fuzzer"
  "//donner/base/fonts:woff_parser_fuzzer"
  "//donner/base/parser:number_parser_fuzzer"
  "//donner/base/xml:xml_parser_fuzzer"
  "//donner/base/xml:xml_parser_structured_fuzzer"
  "//donner/css/parser:anb_microsyntax_parser_fuzzer"
  "//donner/css/parser:color_parser_fuzzer"
  "//donner/css/parser:declaration_list_parser_fuzzer"
  "//donner/css/parser:selector_parser_fuzzer"
  "//donner/css/parser:stylesheet_parser_fuzzer"
  "//donner/svg/parser:animate_motion_path_fuzzer"
  "//donner/svg/parser:animate_transform_value_fuzzer"
  "//donner/svg/parser:animate_value_fuzzer"
  "//donner/svg/parser:clock_value_parser_fuzzer"
  "//donner/svg/parser:list_parser_fuzzer"
  "//donner/svg/parser:path_parser_fuzzer"
  "//donner/svg/parser:svg_parser_fuzzer"
  "//donner/svg/parser:svg_parser_structured_fuzzer"
  "//donner/svg/parser:syncbase_ref_fuzzer"
  "//donner/svg/parser:transform_parser_fuzzer"
  "//donner/svg/resources:url_loader_fuzzer"
)

MAX_PARALLEL=4
MAX_TIME=600  # 10 minutes per fuzzer

# Convert bazel target to binary path
target_to_bin() {
  local target="$1"
  # //donner/base/encoding:decompress_fuzzer -> bazel-bin/donner/base/encoding/decompress_fuzzer
  local path="${target#//}"
  path="${path/://}"
  echo "bazel-bin/$path"
}

run_fuzzer() {
  local target="$1"
  local bin
  bin=$(target_to_bin "$target")
  local name
  name=$(basename "$bin")
  local corpus_dir="/tmp/fuzzer_corpus_${name}"
  mkdir -p "$corpus_dir"

  echo "[$(date +%H:%M:%S)] START: $name"
  if "$bin" "$corpus_dir" -max_total_time=$MAX_TIME -print_final_stats=1 \
      > "$RESULTS_DIR/${name}.log" 2>&1; then
    echo "[$(date +%H:%M:%S)] PASS:  $name"
    echo "PASS" > "$RESULTS_DIR/${name}.status"
  else
    local exit_code=$?
    echo "[$(date +%H:%M:%S)] FAIL:  $name (exit=$exit_code)"
    echo "FAIL(exit=$exit_code)" > "$RESULTS_DIR/${name}.status"
    # Copy crash artifacts if any
    for crash in crash-* leak-* timeout-*; do
      if [ -f "$crash" ]; then
        cp "$crash" "$RESULTS_DIR/${name}_${crash}"
      fi
    done
  fi
}

echo "Running ${#FUZZERS[@]} fuzzers with max_parallel=$MAX_PARALLEL, max_time=${MAX_TIME}s each"
echo "Results will be saved to: $RESULTS_DIR"
echo ""

# Run in parallel batches
running=0
pids=()
for target in "${FUZZERS[@]}"; do
  run_fuzzer "$target" &
  pids+=($!)
  running=$((running + 1))

  if [ "$running" -ge "$MAX_PARALLEL" ]; then
    # Wait for any one process to finish
    wait -n "${pids[@]}" 2>/dev/null || true
    # Clean up finished pids
    new_pids=()
    for pid in "${pids[@]}"; do
      if kill -0 "$pid" 2>/dev/null; then
        new_pids+=("$pid")
      fi
    done
    pids=("${new_pids[@]}")
    running=${#pids[@]}
  fi
done

# Wait for remaining
for pid in "${pids[@]}"; do
  wait "$pid" 2>/dev/null || true
done

echo ""
echo "========================================="
echo "FUZZER RESULTS SUMMARY"
echo "========================================="
passes=0
fails=0
for target in "${FUZZERS[@]}"; do
  local_name=$(basename "$(target_to_bin "$target")")
  status_file="$RESULTS_DIR/${local_name}.status"
  if [ -f "$status_file" ]; then
    status=$(cat "$status_file")
    printf "  %-45s %s\n" "$local_name" "$status"
    if [ "$status" = "PASS" ]; then
      passes=$((passes + 1))
    else
      fails=$((fails + 1))
    fi
  else
    printf "  %-45s %s\n" "$local_name" "NO STATUS"
    fails=$((fails + 1))
  fi
done
echo ""
echo "Total: $passes passed, $fails failed out of ${#FUZZERS[@]}"
echo "========================================="

if [ "$fails" -gt 0 ]; then
  echo ""
  echo "FAILED FUZZERS - see logs in $RESULTS_DIR/"
  exit 1
fi
