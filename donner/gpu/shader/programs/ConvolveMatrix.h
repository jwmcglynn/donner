#pragma once
/// @file
/// SVG convolution parameters and precompiled shader projections.

#include <cstddef>
#include <cstdint>

#include "donner/gpu/shader/CompiledShader.h"

namespace donner::gpu::shader::programs {

/// Host storage layout consumed by the matrix-convolution program. The kernel coefficients are
/// not part of this block: they are bound separately as the runtime-sized `coefficients` array,
/// holding exactly `orderX * orderY` row-major values, so the kernel size is bounded by the host
/// admission limits rather than by a fixed array in the shader.
struct ConvolveMatrixParams {
  int32_t orderX;           //!< Kernel width.
  int32_t orderY;           //!< Kernel height.
  int32_t targetX;          //!< Horizontal target within the kernel.
  int32_t targetY;          //!< Vertical target within the kernel.
  float divisor;            //!< Nonzero scale applied after accumulation.
  float bias;               //!< Offset applied after division.
  uint32_t edgeMode;        //!< Zero duplicates, one wraps, and two samples transparent black.
  uint32_t preserveAlpha;   //!< One retains source alpha and convolves straight RGB.
  int32_t wrapLeft = 0;     //!< First column of the logical input image.
  int32_t wrapTop = 0;      //!< First row of the logical input image.
  int32_t wrapWidth = -1;   //!< Width of the wrap domain. Negative selects the full texture.
  int32_t wrapHeight = -1;  //!< Height of the wrap domain. Negative selects the full texture.
};

static_assert(sizeof(ConvolveMatrixParams) == 48);
static_assert(offsetof(ConvolveMatrixParams, wrapLeft) == 32);

/**
 * Returns the WGSL projection and reflected interface for matrix convolution.
 * The host admits positive kernel orders, targets inside the kernel, finite coefficients, bias and
 * a nonzero divisor, and binds `orderX * orderY` coefficients. The shader rotates the row-major
 * kernel by 180 degrees, supports duplicate/wrap/transparent edges and optionally preserves alpha.
 * @return Stable view into a process-lifetime artifact.
 */
const CompiledShaderView& ConvolveMatrixShader();

/// Returns only the platform's native MSL or SPIR-V projection and the reflected interface.
/// @return Stable view into a process-lifetime artifact.
const CompiledShaderView& ConvolveMatrixNativeShader();

}  // namespace donner::gpu::shader::programs
