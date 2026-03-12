#!/usr/bin/env bash
set -euo pipefail

# Compare filter performance: Skia vs tiny-skia (native SIMD).
# Usage: run via Bazel test or manually:
#   bazel test -c opt //donner/benchmarks:filter_perf_compare --test_output=all

COMPILATION_MODE="$1"
SKIA_BIN="$2"
NATIVE_BIN="$3"

if [[ "${COMPILATION_MODE}" != "opt" ]]; then
  echo "Skipping filter perf comparison: compilation mode is ${COMPILATION_MODE} (need opt)."
  exit 0
fi

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

SKIA_CSV="${TMP_DIR}/skia.csv"
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
  awk -v n="${numerator}" -v d="${denominator}" 'BEGIN { printf "%.2f", n / d }'
}

format_time() {
  local ns="$1"
  awk -v t="${ns}" 'BEGIN { printf "%10.2f ms", t / 1e6 }'
}

echo "Running Skia filter benchmark..."
run_benchmark "${SKIA_BIN}" "${SKIA_CSV}"

echo ""
echo "Running tiny-skia (native) filter benchmark..."
run_benchmark "${NATIVE_BIN}" "${NATIVE_CSV}"

echo ""
echo "======================================================================"
echo "  Filter Performance Comparison: tiny-skia vs Skia"
echo "======================================================================"

print_comparison() {
  local label="$1"
  local native_name="$2"
  local skia_name="$3"

  local native_ns skia_ns native_over_skia

  native_ns="$(extract_mean_real_time_ns "${NATIVE_CSV}" "${native_name}")"
  skia_ns="$(extract_mean_real_time_ns "${SKIA_CSV}" "${skia_name}")"

  if [[ -z "${native_ns}" || -z "${skia_ns}" ]]; then
    printf "  %-40s  MISSING DATA\n" "${label}"
    return
  fi

  native_over_skia="$(ratio "${native_ns}" "${skia_ns}")"

  printf "  %-40s  tiny-skia:%s  skia:%s  ratio: %sx\n" \
    "${label}" \
    "$(format_time "${native_ns}")" \
    "$(format_time "${skia_ns}")" \
    "${native_over_skia}"
}

echo ""
echo "  Gaussian Blur (float)"
echo "  ---------------------"
print_comparison "GaussianBlur 512x512 sigma=3" \
  "BM_GaussianBlur_Float/512/3" "BM_GaussianBlur_Skia/512/3"
print_comparison "GaussianBlur 512x512 sigma=6" \
  "BM_GaussianBlur_Float/512/6" "BM_GaussianBlur_Skia/512/6"
print_comparison "GaussianBlur 512x512 sigma=20" \
  "BM_GaussianBlur_Float/512/20" "BM_GaussianBlur_Skia/512/20"
print_comparison "GaussianBlur 1024x1024 sigma=6" \
  "BM_GaussianBlur_Float/1024/6" "BM_GaussianBlur_Skia/1024/6"

echo ""
echo "  Gaussian Blur (uint8)"
echo "  ---------------------"
print_comparison "GaussianBlur 512x512 sigma=3" \
  "BM_GaussianBlur_Uint8/512/3" "BM_GaussianBlur_Skia/512/3"
print_comparison "GaussianBlur 512x512 sigma=6" \
  "BM_GaussianBlur_Uint8/512/6" "BM_GaussianBlur_Skia/512/6"
print_comparison "GaussianBlur 512x512 sigma=20" \
  "BM_GaussianBlur_Uint8/512/20" "BM_GaussianBlur_Skia/512/20"
print_comparison "GaussianBlur 1024x1024 sigma=6" \
  "BM_GaussianBlur_Uint8/1024/6" "BM_GaussianBlur_Skia/1024/6"

echo ""
echo "  Morphology (dilate)"
echo "  -------------------"
print_comparison "Dilate 512x512 radius=3" \
  "BM_Morphology_Dilate_Float/512/3" "BM_Morphology_Dilate_Skia/512/3"
print_comparison "Dilate 512x512 radius=10" \
  "BM_Morphology_Dilate_Float/512/10" "BM_Morphology_Dilate_Skia/512/10"
print_comparison "Dilate 512x512 radius=30" \
  "BM_Morphology_Dilate_Float/512/30" "BM_Morphology_Dilate_Skia/512/30"

echo ""
echo "  Morphology (erode)"
echo "  ------------------"
print_comparison "Erode 512x512 radius=3" \
  "BM_Morphology_Erode_Float/512/3" "BM_Morphology_Erode_Skia/512/3"
print_comparison "Erode 512x512 radius=10" \
  "BM_Morphology_Erode_Float/512/10" "BM_Morphology_Erode_Skia/512/10"
print_comparison "Erode 512x512 radius=30" \
  "BM_Morphology_Erode_Float/512/30" "BM_Morphology_Erode_Skia/512/30"

echo ""
echo "  Blend"
echo "  -----"
print_comparison "Blend Multiply 512x512" \
  "BM_Blend_Multiply_Float/512" "BM_Blend_Multiply_Skia/512"
print_comparison "Blend Screen 512x512" \
  "BM_Blend_Screen_Float/512" "BM_Blend_Screen_Skia/512"

echo ""
echo "  Composite"
echo "  ---------"
print_comparison "Composite Over 512x512" \
  "BM_Composite_Over_Float/512" "BM_Composite_Over_Skia/512"
print_comparison "Composite Arithmetic 512x512" \
  "BM_Composite_Arithmetic_Float/512" "BM_Composite_Arithmetic_Skia/512"

echo ""
echo "  ColorMatrix"
echo "  -----------"
print_comparison "ColorMatrix Saturate 512x512" \
  "BM_ColorMatrix_Saturate_Float/512" "BM_ColorMatrix_Saturate_Skia/512"

echo ""
echo "  ConvolveMatrix"
echo "  --------------"
print_comparison "ConvolveMatrix 3x3 512x512" \
  "BM_ConvolveMatrix_3x3_Float/512" "BM_ConvolveMatrix_3x3_Skia/512"
print_comparison "ConvolveMatrix 5x5 512x512" \
  "BM_ConvolveMatrix_5x5_Float/512" "BM_ConvolveMatrix_5x5_Skia/512"

echo ""
echo "  Turbulence"
echo "  ----------"
print_comparison "Turbulence 512x512" \
  "BM_Turbulence_Float/512" "BM_Turbulence_Skia/512"
print_comparison "FractalNoise 512x512" \
  "BM_FractalNoise_Float/512" "BM_FractalNoise_Skia/512"

echo ""
echo "  Lighting"
echo "  --------"
print_comparison "DiffuseLighting Point 512x512" \
  "BM_DiffuseLighting_Point_Float/512" "BM_DiffuseLighting_Point_Skia/512"
print_comparison "SpecularLighting Point 512x512" \
  "BM_SpecularLighting_Point_Float/512" "BM_SpecularLighting_Point_Skia/512"

echo ""
echo "  DisplacementMap"
echo "  ---------------"
print_comparison "DisplacementMap 512x512" \
  "BM_DisplacementMap_Float/512" "BM_DisplacementMap_Skia/512"

echo ""
echo "  Flood"
echo "  -----"
print_comparison "Flood 512x512" \
  "BM_Flood_Uint8/512" "BM_Flood_Skia/512"

echo ""
echo "  Offset"
echo "  ------"
print_comparison "Offset 512x512" \
  "BM_Offset_Uint8/512" "BM_Offset_Skia/512"

echo ""
echo "  Merge"
echo "  -----"
print_comparison "Merge 3-Input 512x512" \
  "BM_Merge_3Input_Uint8/512" "BM_Merge_3Input_Skia/512"

echo ""
echo "  ComponentTransfer"
echo "  -----------------"
print_comparison "ComponentTransfer Table 512x512" \
  "BM_ComponentTransfer_Table_Uint8/512" "BM_ComponentTransfer_Table_Skia/512"

echo ""
echo "  Tile"
echo "  ----"
print_comparison "Tile 64x64 512x512" \
  "BM_Tile_Uint8/512" "BM_Tile_Skia/512"

echo ""
echo "  (ratio = tiny-skia / skia; lower is better for tiny-skia)"
echo "======================================================================"
