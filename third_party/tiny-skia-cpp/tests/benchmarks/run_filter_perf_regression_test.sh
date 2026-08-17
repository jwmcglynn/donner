#!/usr/bin/env bash
set -euo pipefail

COMPILATION_MODE="$1"
NATIVE_BIN="$2"
SCALAR_BIN="$3"

# Perf thresholds are calibrated for optimized builds only.
if [[ "${COMPILATION_MODE}" != "opt" ]]; then
  echo "Skipping filter perf regression check: compilation mode is ${COMPILATION_MODE} (need opt)."
  exit 0
fi

ARCH="$(uname -m)"
if [[ "${ARCH}" == "arm64" || "${ARCH}" == "aarch64" ]]; then
  ARCH_KIND="arm64"
elif [[ "${ARCH}" == "x86_64" || "${ARCH}" == "amd64" || "${ARCH}" == "i386" ]]; then
  ARCH_KIND="x86"
else
  echo "Skipping filter perf regression check: unsupported arch ${ARCH}."
  exit 0
fi

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

NATIVE_CSV="${TMP_DIR}/native.csv"

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
  awk -v n="${numerator}" -v d="${denominator}" 'BEGIN { printf "%.6f", n / d }'
}

# Check that a ratio is within [low, high]. Fails the test if outside bounds.
check_ratio() {
  local name="$1"
  local value="$2"
  local low="$3"
  local high="$4"

  awk -v v="${value}" -v lo="${low}" -v hi="${high}" -v n="${name}" '
    BEGIN {
      if (v < lo) {
        printf("FAIL: %s below low-water mark: value=%s, low=%s\n", n, v, lo) > "/dev/stderr"
        exit 1
      }
      if (v > hi) {
        printf("FAIL: %s above high-water mark: value=%s, high=%s\n", n, v, hi) > "/dev/stderr"
        exit 1
      }
    }
  '

  echo "PASS: ${name} value=${value} in [${low}, ${high}]"
}

echo "Running native filter benchmark for perf regression guard..."
run_benchmark "${NATIVE_BIN}" "${NATIVE_CSV}"

# Extract mean times for key benchmarks.
blur_float_s3="$(extract_mean_real_time_ns "${NATIVE_CSV}" "BM_GaussianBlur_Float/512/3")"
blur_float_s6="$(extract_mean_real_time_ns "${NATIVE_CSV}" "BM_GaussianBlur_Float/512/6")"
blur_float_s20="$(extract_mean_real_time_ns "${NATIVE_CSV}" "BM_GaussianBlur_Float/512/20")"
blur_uint8_s6="$(extract_mean_real_time_ns "${NATIVE_CSV}" "BM_GaussianBlur_Uint8/512/6")"
morph_dilate_r3="$(extract_mean_real_time_ns "${NATIVE_CSV}" "BM_Morphology_Dilate_Float/512/3")"
morph_dilate_r10="$(extract_mean_real_time_ns "${NATIVE_CSV}" "BM_Morphology_Dilate_Float/512/10")"
morph_dilate_r30="$(extract_mean_real_time_ns "${NATIVE_CSV}" "BM_Morphology_Dilate_Float/512/30")"
pixmap_from="$(extract_mean_real_time_ns "${NATIVE_CSV}" "BM_FloatPixmap_FromPixmap/512")"
pixmap_to="$(extract_mean_real_time_ns "${NATIVE_CSV}" "BM_FloatPixmap_ToPixmap/512")"
graph_linear="$(extract_mean_real_time_ns "${NATIVE_CSV}" "BM_FilterGraph_SevenPrimitives_LinearRGB/512")"
graph_srgb="$(extract_mean_real_time_ns "${NATIVE_CSV}" "BM_FilterGraph_SevenPrimitives_Srgb/512")"

for value in \
  "${blur_float_s3}" "${blur_float_s6}" "${blur_float_s20}" "${blur_uint8_s6}" \
  "${morph_dilate_r3}" "${morph_dilate_r10}" "${morph_dilate_r30}" \
  "${pixmap_from}" "${pixmap_to}" "${graph_linear}" "${graph_srgb}"; do
  if [[ -z "${value}" ]]; then
    echo "Failed to parse benchmark CSV output" >&2
    exit 1
  fi
done

# --------------------------------------------------------------------------
# Algorithmic invariant checks
# --------------------------------------------------------------------------
# These ratios detect if O(1) algorithms are accidentally replaced with O(n).

# 1. Blur sigma invariance: sigma=20 / sigma=6 should be close to 1.0 (both use
#    O(1) running-sum box blur). If reverted to O(kernelSize), sigma=20 would be ~3x slower.
blur_sigma_ratio="$(ratio "${blur_float_s20}" "${blur_float_s6}")"

# 2. Morphology radius invariance: radius=30 / radius=3 should be moderate (both use
#    O(n) van Herk/Gil-Werman). If reverted to O(r^2), radius=30 would be ~100x slower.
morph_radius_ratio="$(ratio "${morph_dilate_r30}" "${morph_dilate_r3}")"

# 3. Float vs uint8 blur: float should be within ~2x of uint8 (both use same algorithm,
#    float just has wider data). If float path regresses, this ratio would spike.
blur_float_over_uint8="$(ratio "${blur_float_s6}" "${blur_uint8_s6}")"

# 4. Graph color-space conversion overhead: the same seven-primitive stack interpolated in
#    linearRGB vs sRGB. The two run identical primitives on identical data, so the ratio is
#    purely what the executor spends converting color spaces. Converting at the graph's
#    boundaries is two passes no matter how long the graph is; converting around every primitive
#    would be fourteen here and push the ratio past 1.6.
graph_linear_over_srgb="$(ratio "${graph_linear}" "${graph_srgb}")"

echo ""
echo "Computed filter performance metrics:"
echo "  Blur sigma ratio (s20/s6):         ${blur_sigma_ratio}"
echo "  Morphology radius ratio (r30/r3):  ${morph_radius_ratio}"
echo "  Blur float/uint8 ratio (s6):       ${blur_float_over_uint8}"
echo "  Graph linearRGB/sRGB ratio:        ${graph_linear_over_srgb}"
echo ""

# Blur sigma ratio: should be near 1.0 (±25%). Both sigmas use box blur.
check_ratio "blur_sigma_invariance" "${blur_sigma_ratio}" "0.80" "1.30"

# Morphology radius ratio: van Herk has some block-size dependent overhead, so r30 is
# ~1.4x slower than r3. Allow up to 2.0x. If O(r^2) brute-force returns, this would be ~10x+.
check_ratio "morphology_radius_invariance" "${morph_radius_ratio}" "0.80" "2.00"

# Float/uint8 blur ratio: float is ~3x slower due to wider data and uint8's ScaledDivider +
# Vec4u32 SIMD optimization. Allow up to 4.0x.
check_ratio "blur_float_over_uint8" "${blur_float_over_uint8}" "1.00" "4.00"

# Graph conversion overhead: measured at 1.10 with boundary conversion and 1.70 with a conversion
# around every primitive. Allow up to 1.40, which leaves room for machine noise while still
# failing if the per-primitive scheme returns.
check_ratio "graph_linear_over_srgb" "${graph_linear_over_srgb}" "1.00" "1.40"

echo ""
echo "Filter perf regression checks passed."
