#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"

OUT_DIR="${1:-${ROOT_DIR}/.benchmarks}"
mkdir -p "${OUT_DIR}"

NATIVE_CSV="${OUT_DIR}/render_perf_native.csv"
SCALAR_CSV="${OUT_DIR}/render_perf_scalar.csv"
BAZEL_COMPILATION_MODE="${BAZEL_COMPILATION_MODE:-opt}"

run_benchmark() {
  local target="$1"
  local csv_out="$2"

  bazel run -c "${BAZEL_COMPILATION_MODE}" "${target}" -- \
    --benchmark_repetitions=5 \
    --benchmark_report_aggregates_only=true \
    --benchmark_min_time=0.2s \
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

print_workload_summary() {
  local workload_name="$1"
  local native_cpp="$2"
  local scalar_cpp="$3"
  local native_rust="$4"
  local scalar_rust="$5"

  local rust_avg
  rust_avg="$(awk -v a="${native_rust}" -v b="${scalar_rust}" 'BEGIN { printf "%.3f", (a + b) / 2.0 }')"

  local simd_vs_scalar
  simd_vs_scalar="$(awk -v simd="${native_cpp}" -v scalar="${scalar_cpp}" \
    'BEGIN { printf "%.3f", scalar / simd }')"

  local simd_vs_rust
  simd_vs_rust="$(awk -v simd="${native_cpp}" -v rust="${rust_avg}" \
    'BEGIN { printf "%.3f", rust / simd }')"

  local scalar_vs_rust
  scalar_vs_rust="$(awk -v scalar="${scalar_cpp}" -v rust="${rust_avg}" \
    'BEGIN { printf "%.3f", rust / scalar }')"

  cat <<SUMMARY
${workload_name}
  native_cpp_ns:  ${native_cpp}
  scalar_cpp_ns:  ${scalar_cpp}
  rust_avg_ns:    ${rust_avg}
  simd_over_scalar: ${simd_vs_scalar}x
  simd_over_rust:   ${simd_vs_rust}x
  scalar_over_rust: ${scalar_vs_rust}x
SUMMARY
}

echo "Running native benchmark binary..."
run_benchmark "//tests/benchmarks:render_perf_bench_native" "${NATIVE_CSV}"

echo "Running scalar benchmark binary..."
run_benchmark "//tests/benchmarks:render_perf_bench_scalar" "${SCALAR_CSV}"

fill_path_cpp_native="$(extract_mean_real_time_ns "${NATIVE_CSV}" "BM_FillPath_Cpp/512")"
fill_path_cpp_scalar="$(extract_mean_real_time_ns "${SCALAR_CSV}" "BM_FillPath_Cpp/512")"
fill_path_rust_native="$(extract_mean_real_time_ns "${NATIVE_CSV}" "BM_FillPath_Rust/512")"
fill_path_rust_scalar="$(extract_mean_real_time_ns "${SCALAR_CSV}" "BM_FillPath_Rust/512")"

fill_rect_cpp_native="$(extract_mean_real_time_ns "${NATIVE_CSV}" "BM_FillRect_Cpp/512")"
fill_rect_cpp_scalar="$(extract_mean_real_time_ns "${SCALAR_CSV}" "BM_FillRect_Cpp/512")"
fill_rect_rust_native="$(extract_mean_real_time_ns "${NATIVE_CSV}" "BM_FillRect_Rust/512")"
fill_rect_rust_scalar="$(extract_mean_real_time_ns "${SCALAR_CSV}" "BM_FillRect_Rust/512")"

for value in \
  "${fill_path_cpp_native}" "${fill_path_cpp_scalar}" \
  "${fill_path_rust_native}" "${fill_path_rust_scalar}" \
  "${fill_rect_cpp_native}" "${fill_rect_cpp_scalar}" \
  "${fill_rect_rust_native}" "${fill_rect_rust_scalar}"; do
  if [[ -z "${value}" ]]; then
    echo "Failed to parse benchmark CSV output in ${OUT_DIR}" >&2
    exit 1
  fi
done

echo
echo "Results (lower ns is better, speedup > 1 means faster):"
print_workload_summary \
  "FillPath(512x512)" \
  "${fill_path_cpp_native}" \
  "${fill_path_cpp_scalar}" \
  "${fill_path_rust_native}" \
  "${fill_path_rust_scalar}"

echo
print_workload_summary \
  "FillRect(512x512)" \
  "${fill_rect_cpp_native}" \
  "${fill_rect_cpp_scalar}" \
  "${fill_rect_rust_native}" \
  "${fill_rect_rust_scalar}"

echo
echo "CSV artifacts:"
echo "  ${NATIVE_CSV}"
echo "  ${SCALAR_CSV}"
