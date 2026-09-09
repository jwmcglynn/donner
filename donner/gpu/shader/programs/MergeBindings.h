#pragma once
/// @file
/// Binding indices shared by the merge filter program and its hosts.

#include <cstdint>
#include <string_view>

namespace donner::gpu::shader::programs {

/// Compute entry point declared by the program.
inline constexpr std::string_view kMergeEntryPoint = "cs_main";

/// Workgroup width and height shared by the program and dispatch sizing.
inline constexpr uint32_t kMergeWorkgroupSize = 8;

/// Resources in bind group zero.
enum class MergeBinding : uint32_t {
  SourceTexture = 0,       //!< Premultiplied source color.
  DestinationTexture = 1,  //!< Premultiplied backdrop color.
  OutputTexture = 2,       //!< Write-only rgba8unorm result.
};

}  // namespace donner::gpu::shader::programs
