#pragma once
/// @file
/// Binding indices shared by the drop-shadow filter program and its hosts.

#include <cstdint>
#include <string_view>

namespace donner::gpu::shader::programs {

/// Compute entry point declared by the program.
inline constexpr std::string_view kDropShadowEntryPoint = "cs_main";

/// Workgroup width and height shared by the program and dispatch sizing.
inline constexpr uint32_t kDropShadowWorkgroupSize = 8;

/// Resources in bind group zero.
enum class DropShadowBinding : uint32_t {
  SourceTexture = 0,   //!< Premultiplied source color.
  BlurredTexture = 1,  //!< Blurred alpha source.
  OutputTexture = 2,   //!< Write-only rgba32float result.
  Params = 3,          //!< Straight flood color and pixel offset.
};

}  // namespace donner::gpu::shader::programs
