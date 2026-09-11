#pragma once
/// @file
/// Shared blend bindings and dispatch contract.
#include <cstdint>
#include <string_view>
namespace donner::gpu::shader::programs {
/// Compute entry point.
inline constexpr std::string_view kBlendEntryPoint = "cs_main";
/// Workgroup width and height.
inline constexpr uint32_t kBlendWorkgroupSize = 8;
/// Resources in group zero.
enum class BlendBinding : uint32_t {
  SourceTexture = 0,       //!< Premultiplied source.
  DestinationTexture = 1,  //!< Premultiplied backdrop.
  OutputTexture = 2,       //!< Write-only rgba32float output.
  Params = 3,              //!< Mode index followed by three padding words.
};
}  // namespace donner::gpu::shader::programs
