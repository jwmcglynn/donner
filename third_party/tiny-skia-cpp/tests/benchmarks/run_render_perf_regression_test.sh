#!/usr/bin/env bash
set -euo pipefail

COMPILATION_MODE="$1"
NATIVE_BIN="$2"
SCALAR_BIN="$3"

# Perf thresholds are calibrated for optimized builds only.
if [[ "${COMPILATION_MODE}" != "opt" ]]; then
  echo "Skipping perf regression check: compilation mode is ${COMPILATION_MODE} (need opt)."
  exit 0
fi

ARCH="$(uname -m)"
if [[ "${ARCH}" == "arm64" || "${ARCH}" == "aarch64" ]]; then
  ARCH_KIND="arm64"
elif [[ "${ARCH}" == "x86_64" || "${ARCH}" == "amd64" || "${ARCH}" == "i386" ]]; then
  ARCH_KIND="x86"
else
  echo "Skipping perf regression check: unsupported arch ${ARCH}."
  exit 0
fi

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

NATIVE_CSV="${TMP_DIR}/native.csv"
SCALAR_CSV="${TMP_DIR}/scalar.csv"

run_benchmark() {
  local bin="$1"
  local csv_out="$2"

  "${bin}" \
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

ratio() {
  local numerator="$1"
  local denominator="$2"
  awk -v n="${numerator}" -v d="${denominator}" 'BEGIN { printf "%.6f", n / d }'
}

baseline_ratio() {
  local arch="$1"
  local workload="$2"
  local metric="$3"

  if [[ "${arch}" == "arm64" && "${workload}" == "fill_path" \
    && "${metric}" == "simd_over_scalar" ]]; then
    echo "2.20"; return
  fi
  if [[ "${arch}" == "arm64" && "${workload}" == "fill_path" \
    && "${metric}" == "simd_over_rust" ]]; then
    echo "2.00"; return
  fi

  if [[ "${arch}" == "arm64" && "${workload}" == "fill_rect" \
    && "${metric}" == "simd_over_scalar" ]]; then
    echo "3.20"; return
  fi
  if [[ "${arch}" == "arm64" && "${workload}" == "fill_rect" \
    && "${metric}" == "simd_over_rust" ]]; then
    echo "2.80"; return
  fi

  if [[ "${arch}" == "arm64" && "${workload}" == "stroke_path" \
    && "${metric}" == "simd_over_scalar" ]]; then
    echo "1.20"; return
  fi
  if [[ "${arch}" == "arm64" && "${workload}" == "fill_path_gradient" \
    && "${metric}" == "simd_over_scalar" ]]; then
    echo "1.40"; return
  fi
  if [[ "${arch}" == "arm64" && "${workload}" == "fill_path_opaque" \
    && "${metric}" == "simd_over_scalar" ]]; then
    echo "1.25"; return
  fi

  if [[ "${arch}" == "x86" && "${workload}" == "fill_path" \
    && "${metric}" == "simd_over_scalar" ]]; then
    echo "1.85"; return
  fi
  if [[ "${arch}" == "x86" && "${workload}" == "fill_path" \
    && "${metric}" == "simd_over_rust" ]]; then
    echo "1.18"; return
  fi

  if [[ "${arch}" == "x86" && "${workload}" == "fill_rect" \
    && "${metric}" == "simd_over_scalar" ]]; then
    echo "2.26"; return
  fi
  if [[ "${arch}" == "x86" && "${workload}" == "fill_rect" \
    && "${metric}" == "simd_over_rust" ]]; then
    echo "1.29"; return
  fi

  if [[ "${arch}" == "x86" && "${workload}" == "stroke_path" \
    && "${metric}" == "simd_over_scalar" ]]; then
    echo "1.28"; return
  fi
  if [[ "${arch}" == "x86" && "${workload}" == "fill_path_gradient" \
    && "${metric}" == "simd_over_scalar" ]]; then
    echo "2.12"; return
  fi
  if [[ "${arch}" == "x86" && "${workload}" == "fill_path_opaque" \
    && "${metric}" == "simd_over_scalar" ]]; then
    echo "1.00"; return
  fi

  echo ""
}

threshold_bounds() {
  local arch="$1"
  local workload="$2"
  local metric="$3"

  local baseline
  baseline="$(baseline_ratio "${arch}" "${workload}" "${metric}")"
  if [[ -z "${baseline}" ]]; then
    echo ""
    return
  fi

  awk -v b="${baseline}" 'BEGIN { printf "%.6f %.6f", b * 0.75, b * 1.25 }'
}

check_metric() {
  local arch="$1"
  local workload="$2"
  local metric="$3"
  local value="$4"

  local bounds
  bounds="$(threshold_bounds "${arch}" "${workload}" "${metric}")"
  if [[ -z "${bounds}" ]]; then
    echo "Missing thresholds for arch=${arch}, workload=${workload}, metric=${metric}" >&2
    exit 1
  fi

  local low high
  read -r low high <<<"${bounds}"

  awk -v v="${value}" -v lo="${low}" -v hi="${high}" -v a="${arch}" -v w="${workload}" \
    -v m="${metric}" '
      BEGIN {
        if (v < lo) {
          printf("FAIL: metric below low-water mark (%s/%s/%s): value=%s, low=%s\n", w, m, a, v, lo) > "/dev/stderr"
          exit 1
        }
        if (v > hi) {
          printf("FAIL: metric above high-water mark (%s/%s/%s): value=%s, high=%s\n", w, m, a, v, hi) > "/dev/stderr"
          exit 1
        }
      }
    '

  echo "PASS: ${workload}/${metric}/${arch} value=${value} in [${low}, ${high}]"
}

calc_rust_avg() {
  local native_rust="$1"
  local scalar_rust="$2"
  awk -v n="${native_rust}" -v s="${scalar_rust}" 'BEGIN { printf "%.6f", (n + s) / 2.0 }'
}

echo "Running native benchmark binary for perf regression guard..."
run_benchmark "${NATIVE_BIN}" "${NATIVE_CSV}"

echo "Running scalar benchmark binary for perf regression guard..."
run_benchmark "${SCALAR_BIN}" "${SCALAR_CSV}"

fill_path_cpp_native="$(extract_mean_real_time_ns "${NATIVE_CSV}" "BM_FillPath_Cpp/512")"
fill_path_cpp_scalar="$(extract_mean_real_time_ns "${SCALAR_CSV}" "BM_FillPath_Cpp/512")"
fill_path_rust_native="$(extract_mean_real_time_ns "${NATIVE_CSV}" "BM_FillPath_Rust/512")"
fill_path_rust_scalar="$(extract_mean_real_time_ns "${SCALAR_CSV}" "BM_FillPath_Rust/512")"

fill_rect_cpp_native="$(extract_mean_real_time_ns "${NATIVE_CSV}" "BM_FillRect_Cpp/512")"
fill_rect_cpp_scalar="$(extract_mean_real_time_ns "${SCALAR_CSV}" "BM_FillRect_Cpp/512")"
fill_rect_rust_native="$(extract_mean_real_time_ns "${NATIVE_CSV}" "BM_FillRect_Rust/512")"
fill_rect_rust_scalar="$(extract_mean_real_time_ns "${SCALAR_CSV}" "BM_FillRect_Rust/512")"

stroke_path_cpp_native="$(extract_mean_real_time_ns "${NATIVE_CSV}" "BM_StrokePath_Cpp/512")"
stroke_path_cpp_scalar="$(extract_mean_real_time_ns "${SCALAR_CSV}" "BM_StrokePath_Cpp/512")"

fill_path_gradient_cpp_native="$(extract_mean_real_time_ns "${NATIVE_CSV}" "BM_FillPath_LinearGradient_Cpp/512")"
fill_path_gradient_cpp_scalar="$(extract_mean_real_time_ns "${SCALAR_CSV}" "BM_FillPath_LinearGradient_Cpp/512")"

fill_path_opaque_cpp_native="$(extract_mean_real_time_ns "${NATIVE_CSV}" "BM_FillPath_Opaque_Cpp/512")"
fill_path_opaque_cpp_scalar="$(extract_mean_real_time_ns "${SCALAR_CSV}" "BM_FillPath_Opaque_Cpp/512")"

for value in \
  "${fill_path_cpp_native}" "${fill_path_cpp_scalar}" \
  "${fill_path_rust_native}" "${fill_path_rust_scalar}" \
  "${fill_rect_cpp_native}" "${fill_rect_cpp_scalar}" \
  "${fill_rect_rust_native}" "${fill_rect_rust_scalar}" \
  "${stroke_path_cpp_native}" "${stroke_path_cpp_scalar}" \
  "${fill_path_gradient_cpp_native}" "${fill_path_gradient_cpp_scalar}" \
  "${fill_path_opaque_cpp_native}" "${fill_path_opaque_cpp_scalar}"; do
  if [[ -z "${value}" ]]; then
    echo "Failed to parse benchmark CSV output" >&2
    exit 1
  fi
done

fill_path_rust_avg="$(calc_rust_avg "${fill_path_rust_native}" "${fill_path_rust_scalar}")"
fill_rect_rust_avg="$(calc_rust_avg "${fill_rect_rust_native}" "${fill_rect_rust_scalar}")"

fill_path_simd_over_scalar="$(ratio "${fill_path_cpp_scalar}" "${fill_path_cpp_native}")"
fill_path_simd_over_rust="$(ratio "${fill_path_rust_avg}" "${fill_path_cpp_native}")"

fill_rect_simd_over_scalar="$(ratio "${fill_rect_cpp_scalar}" "${fill_rect_cpp_native}")"
fill_rect_simd_over_rust="$(ratio "${fill_rect_rust_avg}" "${fill_rect_cpp_native}")"

stroke_path_simd_over_scalar="$(ratio "${stroke_path_cpp_scalar}" "${stroke_path_cpp_native}")"
fill_path_gradient_simd_over_scalar="$(ratio "${fill_path_gradient_cpp_scalar}" "${fill_path_gradient_cpp_native}")"
fill_path_opaque_simd_over_scalar="$(ratio "${fill_path_opaque_cpp_scalar}" "${fill_path_opaque_cpp_native}")"

echo "Computed metrics (matching run_render_perf_compare.sh):"
echo "  FillPath simd_over_scalar=${fill_path_simd_over_scalar} simd_over_rust=${fill_path_simd_over_rust}"
echo "  FillRect simd_over_scalar=${fill_rect_simd_over_scalar} simd_over_rust=${fill_rect_simd_over_rust}"
echo "  StrokePath simd_over_scalar=${stroke_path_simd_over_scalar}"
echo "  FillPathGradient simd_over_scalar=${fill_path_gradient_simd_over_scalar}"
echo "  FillPathOpaque simd_over_scalar=${fill_path_opaque_simd_over_scalar}"

check_metric "${ARCH_KIND}" "fill_path" "simd_over_scalar" "${fill_path_simd_over_scalar}"
check_metric "${ARCH_KIND}" "fill_path" "simd_over_rust" "${fill_path_simd_over_rust}"

check_metric "${ARCH_KIND}" "fill_rect" "simd_over_scalar" "${fill_rect_simd_over_scalar}"
check_metric "${ARCH_KIND}" "fill_rect" "simd_over_rust" "${fill_rect_simd_over_rust}"

check_metric "${ARCH_KIND}" "stroke_path" "simd_over_scalar" "${stroke_path_simd_over_scalar}"
check_metric "${ARCH_KIND}" "fill_path_gradient" "simd_over_scalar" "${fill_path_gradient_simd_over_scalar}"
check_metric "${ARCH_KIND}" "fill_path_opaque" "simd_over_scalar" "${fill_path_opaque_simd_over_scalar}"

echo "Perf regression ratios are within configured bounds."
