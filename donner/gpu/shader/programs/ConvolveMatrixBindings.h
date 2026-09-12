#pragma once
/// @file
/// Bindings shared by the SVG convolution program and its host without linking the shader IR.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace donner::gpu::shader::programs {

/// Compute entry point.
inline constexpr std::string_view kConvolveMatrixEntryPoint = "cs_main";

/// Square compute workgroup extent.
inline constexpr uint32_t kConvolveMatrixWorkgroupSize = 8;

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

/// Resource bindings for matrix convolution.
enum class ConvolveMatrixBinding : uint32_t {
  InputTexture = 0,   //!< Sampled source.
  OutputTexture = 1,  //!< Float storage destination.
  Params = 2,         //!< Header and tightly packed kernel coefficients.
};

}  // namespace donner::gpu::shader::programs
