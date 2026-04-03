#!/usr/bin/env bash
set -euo pipefail

# Compare render performance: Skia vs tiny-skia.
# Usage: run via Bazel test or manually:
#   bazel test -c opt //donner/benchmarks:render_perf_compare --test_output=all

COMPILATION_MODE="$1"
SKIA_BIN="$2"
TINYSKIA_BIN="$3"

if [[ "${COMPILATION_MODE}" != "opt" ]]; then
  echo "Skipping render perf comparison: compilation mode is ${COMPILATION_MODE} (need opt)."
  exit 0
fi

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

SKIA_CSV="${TMP_DIR}/skia.csv"
TINYSKIA_CSV="${TMP_DIR}/tinyskia.csv"

run_benchmark() {
  local bin="$1"
  local csv_out="$2"

  "${bin}" \
    --benchmark_repetitions=5 \
    --benchmark_report_aggregates_only=true \
    --benchmark_min_time=0.3s \
    --benchmark_time_unit=ns \
    --benchmark_out="${csv_out}" \
    --benchmark_out_format=csv
}

extract_mean_real_time_ns() {
  local csv_file="$1"
  local benchmark_name="$2"

  awk -F, -v name="${benchmark_name}" '
    {
      key = $1
      gsub(/^"/, "", key)
      gsub(/"$/, "", key)
    }
    key == name "_mean" { print $3; found = 1; exit }
    key == name { fallback = $3 }
    END {
      if (!found && fallback != "") {
        print fallback
      }
    }
  ' "${csv_file}"
}

ratio() {
  local numerator="$1"
  local denominator="$2"
  awk -v n="${numerator}" -v d="${denominator}" 'BEGIN { printf "%.2f", n / d }'
}

format_time() {
  local ns="$1"
  awk -v t="${ns}" 'BEGIN { printf "%10.2f ms", t / 1e6 }'
}

echo "Running Skia render benchmark..."
run_benchmark "${SKIA_BIN}" "${SKIA_CSV}"

echo ""
echo "Running tiny-skia render benchmark..."
run_benchmark "${TINYSKIA_BIN}" "${TINYSKIA_CSV}"

echo ""
echo "======================================================================"
echo "  Render Performance Comparison: tiny-skia vs Skia"
echo "======================================================================"

print_comparison() {
  local label="$1"
  local tinyskia_name="$2"
  local skia_name="$3"

  local tinyskia_ns skia_ns tinyskia_over_skia

  tinyskia_ns="$(extract_mean_real_time_ns "${TINYSKIA_CSV}" "${tinyskia_name}")"
  skia_ns="$(extract_mean_real_time_ns "${SKIA_CSV}" "${skia_name}")"

  if [[ -z "${tinyskia_ns}" || -z "${skia_ns}" ]]; then
    printf "  %-40s  MISSING DATA\n" "${label}"
    return
  fi

  tinyskia_over_skia="$(ratio "${tinyskia_ns}" "${skia_ns}")"

  printf "  %-40s  tiny-skia:%s  skia:%s  ratio: %sx\n" \
    "${label}" \
    "$(format_time "${tinyskia_ns}")" \
    "$(format_time "${skia_ns}")" \
    "${tinyskia_over_skia}"
}

echo ""
echo "  Fill Operations (512x512)"
echo "  -------------------------"
print_comparison "FillPath (semi-transparent)" \
  "BM_FillPath_TinySkia/512" "BM_FillPath_Skia/512"
print_comparison "FillRect (semi-transparent)" \
  "BM_FillRect_TinySkia/512" "BM_FillRect_Skia/512"
print_comparison "FillPath Opaque" \
  "BM_FillPath_Opaque_TinySkia/512" "BM_FillPath_Opaque_Skia/512"
print_comparison "FillPath EvenOdd" \
  "BM_FillPath_EvenOdd_TinySkia/512" "BM_FillPath_EvenOdd_Skia/512"
print_comparison "FillPath Transformed (30deg)" \
  "BM_FillPath_Transformed_TinySkia/512" "BM_FillPath_Transformed_Skia/512"

echo ""
echo "  Stroke Operations (512x512)"
echo "  ---------------------------"
print_comparison "StrokePath (3px round)" \
  "BM_StrokePath_TinySkia/512" "BM_StrokePath_Skia/512"
print_comparison "StrokePath Dashed (10/5)" \
  "BM_StrokePath_Dashed_TinySkia/512" "BM_StrokePath_Dashed_Skia/512"
print_comparison "StrokePath Thick (10px round)" \
  "BM_StrokePath_Thick_TinySkia/512" "BM_StrokePath_Thick_Skia/512"

echo ""
echo "  Shader Operations (512x512)"
echo "  ---------------------------"
print_comparison "FillPath LinearGradient" \
  "BM_FillPath_LinearGradient_TinySkia/512" "BM_FillPath_LinearGradient_Skia/512"
print_comparison "FillPath RadialGradient" \
  "BM_FillPath_RadialGradient_TinySkia/512" "BM_FillPath_RadialGradient_Skia/512"
print_comparison "FillPath Pattern (64x64 tile)" \
  "BM_FillPath_Pattern_TinySkia/512" "BM_FillPath_Pattern_Skia/512"

echo ""
echo "  (ratio = tiny-skia / skia; lower is better for tiny-skia)"
echo "======================================================================"

# -----------------------------------------------------------------------
# Enforce 1.5x threshold: fail if any render operation exceeds 1.5x of Skia.
# -----------------------------------------------------------------------
MAX_RATIO="1.50"
FAIL_COUNT=0

check_threshold() {
  local label="$1"
  local tinyskia_name="$2"
  local skia_name="$3"

  local tinyskia_ns skia_ns r
  tinyskia_ns="$(extract_mean_real_time_ns "${TINYSKIA_CSV}" "${tinyskia_name}")"
  skia_ns="$(extract_mean_real_time_ns "${SKIA_CSV}" "${skia_name}")"

  if [[ -z "${tinyskia_ns}" || -z "${skia_ns}" ]]; then
    return
  fi

  r="$(ratio "${tinyskia_ns}" "${skia_ns}")"

  local exceeded
  exceeded="$(awk -v r="${r}" -v max="${MAX_RATIO}" 'BEGIN { print (r > max) ? "yes" : "no" }')"
  if [[ "${exceeded}" == "yes" ]]; then
    echo "FAIL: ${label} ratio ${r}x exceeds ${MAX_RATIO}x threshold"
    FAIL_COUNT=$((FAIL_COUNT + 1))
  fi
}

echo ""
echo "Checking 1.5x threshold..."

check_threshold "FillPath (semi-transparent)" \
  "BM_FillPath_TinySkia/512" "BM_FillPath_Skia/512"
check_threshold "FillRect (semi-transparent)" \
  "BM_FillRect_TinySkia/512" "BM_FillRect_Skia/512"
check_threshold "FillPath Opaque" \
  "BM_FillPath_Opaque_TinySkia/512" "BM_FillPath_Opaque_Skia/512"
check_threshold "FillPath EvenOdd" \
  "BM_FillPath_EvenOdd_TinySkia/512" "BM_FillPath_EvenOdd_Skia/512"
check_threshold "FillPath Transformed (30deg)" \
  "BM_FillPath_Transformed_TinySkia/512" "BM_FillPath_Transformed_Skia/512"
check_threshold "StrokePath (3px round)" \
  "BM_StrokePath_TinySkia/512" "BM_StrokePath_Skia/512"
check_threshold "StrokePath Dashed (10/5)" \
  "BM_StrokePath_Dashed_TinySkia/512" "BM_StrokePath_Dashed_Skia/512"
check_threshold "StrokePath Thick (10px round)" \
  "BM_StrokePath_Thick_TinySkia/512" "BM_StrokePath_Thick_Skia/512"
check_threshold "FillPath LinearGradient" \
  "BM_FillPath_LinearGradient_TinySkia/512" "BM_FillPath_LinearGradient_Skia/512"
check_threshold "FillPath RadialGradient" \
  "BM_FillPath_RadialGradient_TinySkia/512" "BM_FillPath_RadialGradient_Skia/512"
check_threshold "FillPath Pattern (64x64 tile)" \
  "BM_FillPath_Pattern_TinySkia/512" "BM_FillPath_Pattern_Skia/512"

if [[ "${FAIL_COUNT}" -gt 0 ]]; then
  echo ""
  echo "FAILED: ${FAIL_COUNT} render operation(s) exceeded the ${MAX_RATIO}x threshold."
  exit 1
fi

echo "PASSED: All render operations within ${MAX_RATIO}x of Skia."
