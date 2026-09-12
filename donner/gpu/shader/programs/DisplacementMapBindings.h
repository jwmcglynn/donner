#pragma once
/// @file
/// Shared displacement-map shader binding and dispatch contract.

#include <cstdint>
#include <string_view>

namespace donner::gpu::shader::programs {

/// Compute entry point.
inline constexpr std::string_view kDisplacementMapEntryPoint = "cs_main";

/// Workgroup width and height.
inline constexpr uint32_t kDisplacementMapWorkgroupSize = 8;

/// Resources in bind group zero.
enum class DisplacementMapBinding : uint32_t {
  SourceTexture = 0,  //!< Premultiplied source.
  MapTexture = 1,     //!< Premultiplied displacement channels.
  OutputTexture = 2,  //!< Write-only rgba32float output.
  Params = 3,         //!< Scale, x selector, y selector, padding.
};

}  // namespace donner::gpu::shader::programs
