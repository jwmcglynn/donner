#pragma once
/// @file
/// Bindings shared by the SVG tile program and its host without linking the shader IR.
#include <cstdint>
#include <string_view>
namespace donner::gpu::shader::programs {
/// Compute entry point.
inline constexpr std::string_view kTileEntryPoint = "cs_main";
/// Square compute workgroup extent.
inline constexpr uint32_t kTileWorkgroupSize = 8;
/// Resource bindings for subregion repetition.
enum class TileBinding : uint32_t {
  InputTexture = 0,   //!< Sampled source.
  OutputTexture = 1,  //!< Float storage destination.
  Params = 2,         //!< Signed source origin and extent in pixels.
};
}  // namespace donner::gpu::shader::programs
