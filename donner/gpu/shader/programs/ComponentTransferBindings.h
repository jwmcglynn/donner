#pragma once
/// @file
/// Bindings shared by the SVG component-transfer program and its host without linking the shader
/// IR.
#include <cstdint>
#include <string_view>
namespace donner::gpu::shader::programs {
/// Compute entry point.
inline constexpr std::string_view kComponentTransferEntryPoint = "cs_main";
/// Square compute workgroup extent.
inline constexpr uint32_t kComponentTransferWorkgroupSize = 8;
/// Resource bindings for channel transfer.
enum class ComponentTransferBinding : uint32_t {
  InputTexture = 0,   //!< Sampled source.
  OutputTexture = 1,  //!< Float storage destination.
  Params = 2,         //!< Read-only storage block with four functions and bounded tables.
};
}  // namespace donner::gpu::shader::programs
