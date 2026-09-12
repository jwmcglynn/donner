#pragma once
/// @file
/// Bindings shared by the SVG image filter program and its host.

#include <cstdint>
#include <string_view>

namespace donner::gpu::shader::programs {

/// Compute entry point.
inline constexpr std::string_view kFilterImageEntryPoint = "cs_main";

/// Square compute workgroup extent.
inline constexpr uint32_t kFilterImageWorkgroupSize = 8;

/// Resource bindings for image sampling and placement.
enum class FilterImageBinding : uint32_t {
  ImageTexture = 0,   //!< Premultiplied sampled source.
  OutputTexture = 1,  //!< Float storage destination.
  Params = 2,         //!< Image-from-output transform and sampling parameters.
};

}  // namespace donner::gpu::shader::programs
