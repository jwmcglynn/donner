#pragma once
/// @file
/// SVG offset parameters and precompiled shader projections.

#include <cmath>
#include <cstdint>

#include "donner/gpu/shader/CompiledShader.h"

namespace donner::gpu::shader::programs {

/// Whole-pixel base and constant blend weight of a pixel shift along one axis: output pixel x
/// blends input pixels x - shift and x - shift + 1 by weight. The split is made once, in double
/// precision, so every pixel of a translation uses the same weight wherever the raster sits.
struct PixelShiftSplit {
  int32_t shift;  //!< Input pixel x - shift is the first of the two blended pixels.
  float weight;   //!< Weight of input pixel x - shift + 1; zero for a whole-pixel shift.
};

/// Splits a finite pixel shift in the admitted [-4096, 4096] range.
/// @param pixels Shift in pixels; positive moves content toward larger coordinates.
inline PixelShiftSplit SplitPixelShift(double pixels) {
  const double whole = std::floor(pixels);
  const double fraction = pixels - whole;
  if (fraction == 0.0) {
    return {static_cast<int32_t>(whole), 0.0f};
  }
  return {static_cast<int32_t>(whole) + 1, static_cast<float>(1.0 - fraction)};
}

/// Uniform layout for finite pixel shifts in the admitted [-4096, 4096] range, split per axis by
/// \ref SplitPixelShift.
struct OffsetParams {
  int32_t shiftX;  //!< Whole-pixel base along x.
  int32_t shiftY;  //!< Whole-pixel base along y.
  float weightX;   //!< Constant blend weight along x.
  float weightY;   //!< Constant blend weight along y.
};
static_assert(sizeof(OffsetParams) == 16);

/// Builds the uniform for a shift of (\p dx, \p dy) pixels.
inline OffsetParams MakeOffsetParams(double dx, double dy) {
  const PixelShiftSplit x = SplitPixelShift(dx);
  const PixelShiftSplit y = SplitPixelShift(dy);
  return OffsetParams{x.shift, y.shift, x.weight, y.weight};
}

/// Returns the authored WGSL offset shader and its reflected resource/compute interface.
/// Source texels outside the input extent become transparent black. Invocations outside the
/// destination extent do not write. Callers provide finite shifts in [-4096, 4096].
/// @return Stable view into a process-lifetime artifact.
const CompiledShaderView& OffsetShader();

/// Returns only the platform's native MSL or SPIR-V projection and the reflected interface.
/// @return Stable view into a process-lifetime artifact.
const CompiledShaderView& OffsetNativeShader();

}  // namespace donner::gpu::shader::programs
