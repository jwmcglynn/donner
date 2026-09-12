#pragma once
/// @file
/// Bindings shared by the SVG morphology program and its host without linking the shader IR.
#include <cstdint>
#include <string_view>
namespace donner::gpu::shader::programs {
/// Compute entry point.
inline constexpr std::string_view kMorphologyEntryPoint = "cs_main";
/// Square compute workgroup extent.
inline constexpr uint32_t kMorphologyWorkgroupSize = 8;
/// Resource bindings for neighborhood erosion or dilation.
enum class MorphologyBinding : uint32_t {
  InputTexture = 0,   //!< Sampled source.
  OutputTexture = 1,  //!< Float storage destination.
  Params = 2,         //!< Signed x/y radii, operation, and padding.
};
}  // namespace donner::gpu::shader::programs
