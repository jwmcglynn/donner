#pragma once
/// @file
#include <array>
#include <cstdint>

#include "donner/gpu/shader/CompiledShader.h"
namespace donner::gpu::shader::programs {
/**
 * Host layout for the `params` uniform resource of the Gaussian blur program.
 */
struct alignas(8) GaussianBlurParams {
  float stdDeviation = 0.0f;         //!< Gaussian standard deviation in pixels.
  uint32_t axis = 0;                 //!< Zero for horizontal and one for vertical sampling.
  uint32_t edgeMode = 0;             //!< Transparent, duplicate, or wrap edge behavior.
  uint32_t kernelType = 0;           //!< Gaussian or box kernel selection.
  int32_t boxLeft = 0;               //!< Inclusive box-kernel extent below zero.
  int32_t boxRight = 0;              //!< Inclusive box-kernel extent above zero.
  std::array<int32_t, 2> clipMin{};  //!< Inclusive output clip origin.
  std::array<int32_t, 2> clipMax{};  //!< Exclusive output clip limit.
  uint32_t clipActive = 0;           //!< Enables output clipping when one.
  uint32_t pad = 0;                  //!< Explicit trailing uniform word.
};

/**
 * Returns the WGSL projection and reflected interface used by the Geode backend.
 * The filter admission path bounds finite sigma to [0,256], box extents to [0,240], and
 * supplies equally sized nonempty source and destination textures. The shader caps Gaussian
 * support at 127 pixels and applies its optional output clip before the [0,1] output clamp.
 *
 * @return Stable view into a process-lifetime compiled artifact.
 */
const CompiledShaderView& GaussianBlurShader();

/**
 * Returns the native projection and reflected interface used by Metal or Vulkan.
 *
 * @return Stable view into a process-lifetime compiled artifact.
 */
const CompiledShaderView& GaussianBlurNativeShader();
}  // namespace donner::gpu::shader::programs
