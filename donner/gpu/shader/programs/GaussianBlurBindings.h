#pragma once
/// @file
/// Bindings shared by the SVG Gaussian blur program and its host without linking the shader IR.
#include <cstdint>
#include <string_view>
namespace donner::gpu::shader::programs {
/// Compute entry point.
inline constexpr std::string_view kGaussianBlurEntryPoint = "cs_main";
/// Square compute workgroup extent.
inline constexpr uint32_t kGaussianBlurWorkgroupSize = 8;
/// Resource bindings for separable blur.
enum class GaussianBlurBinding : uint32_t {
  InputTexture = 0,   //!< Sampled source.
  OutputTexture = 1,  //!< Float storage destination.
  Params = 2,         //!< Kernel, edge mode, axis, and optional output clip.
};
}  // namespace donner::gpu::shader::programs
