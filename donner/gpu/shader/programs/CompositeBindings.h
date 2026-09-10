#pragma once
/// @file
/// Binding indices shared by the composite filter program and its hosts.

#include <cstdint>
#include <string_view>

namespace donner::gpu::shader::programs {

/// Compute entry point declared by the program.
inline constexpr std::string_view kCompositeEntryPoint = "cs_main";

/// Workgroup width and height shared by the program and dispatch sizing.
inline constexpr uint32_t kCompositeWorkgroupSize = 8;

/// Resources in bind group zero.
enum class CompositeBinding : uint32_t {
  SourceTexture = 0,       //!< Premultiplied source color.
  DestinationTexture = 1,  //!< Premultiplied backdrop color.
  OutputTexture = 2,       //!< Write-only rgba32float result.
  Params = 3,              //!< Operator and arithmetic coefficients.
};

/// SVG compositing operators as encoded in the uniform block.
enum class CompositeOperator : uint32_t {
  Over = 0,        //!< Source over destination.
  In = 1,          //!< Source inside destination.
  Out = 2,         //!< Source outside destination.
  Atop = 3,        //!< Source atop destination.
  Xor = 4,         //!< Nonoverlapping source and destination.
  Lighter = 5,     //!< Saturating sum.
  Arithmetic = 6,  //!< Weighted product, sum, and constant.
};

}  // namespace donner::gpu::shader::programs
