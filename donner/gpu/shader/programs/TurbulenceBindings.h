#pragma once
/// @file
/// Shared turbulence shader binding, table, and dispatch contract.

#include <cstdint>
#include <string_view>

namespace donner::gpu::shader::programs {

/// Compute entry point.
inline constexpr std::string_view kTurbulenceEntryPoint = "cs_main";

/// Workgroup width and height.
inline constexpr uint32_t kTurbulenceWorkgroupSize = 8;

/// Permutation and gradient table lengths required by the SVG noise algorithm.
inline constexpr uint32_t kTurbulenceBaseTableSize = 256;
inline constexpr uint32_t kTurbulenceTableSize = 514;
inline constexpr uint32_t kTurbulenceGradientTableSize = 4 * kTurbulenceTableSize;

/// Resources in bind group zero.
enum class TurbulenceBinding : uint32_t {
  OutputTexture = 0,  //!< Write-only rgba32float output.
  Params = 1,         //!< Read-only storage block with noise and coordinate parameters.
  Tables = 2,         //!< Read-only storage block with permutation and gradient tables.
};

}  // namespace donner::gpu::shader::programs
