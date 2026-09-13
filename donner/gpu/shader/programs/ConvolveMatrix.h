#pragma once
/// @file
/// SVG convolution parameters and precompiled shader projections.

#include <cstddef>
#include <cstdint>

#include "donner/gpu/shader/CompiledShader.h"

namespace donner::gpu::shader::programs {

/// Maximum number of tightly packed kernel coefficients in the parameter storage block.
inline constexpr uint32_t kConvolveMatrixKernelCapacity = 25;

/// Host storage layout consumed by the matrix-convolution program.
struct ConvolveMatrixParams {
  int32_t orderX;          //!< Kernel width.
  int32_t orderY;          //!< Kernel height.
  int32_t targetX;         //!< Horizontal target within the kernel.
  int32_t targetY;         //!< Vertical target within the kernel.
  float divisor;           //!< Nonzero scale applied after accumulation.
  float bias;              //!< Offset applied after division.
  uint32_t edgeMode;       //!< Zero duplicates, one wraps, and two samples transparent black.
  uint32_t preserveAlpha;  //!< One retains source alpha and convolves straight RGB.
  float kernel[kConvolveMatrixKernelCapacity];  //!< Coefficients rotated when sampled.
};

static_assert(sizeof(ConvolveMatrixParams) == 132);
static_assert(offsetof(ConvolveMatrixParams, kernel) == 32);

/**
 * Returns the WGSL projection and reflected interface for matrix convolution.
 * The host admits positive kernel orders whose product fits the coefficient array, targets inside
 * the kernel, and finite coefficients, bias and nonzero divisor. The shader rotates the row-major
 * kernel by 180 degrees, supports duplicate/wrap/transparent edges and optionally preserves alpha.
 * @return Stable view into a process-lifetime artifact.
 */
const CompiledShaderView& ConvolveMatrixShader();

/// Returns only the platform's native MSL or SPIR-V projection and the reflected interface.
/// @return Stable view into a process-lifetime artifact.
const CompiledShaderView& ConvolveMatrixNativeShader();

}  // namespace donner::gpu::shader::programs
