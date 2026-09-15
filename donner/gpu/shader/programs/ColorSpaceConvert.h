#pragma once
/// @file
/// Color-space conversion parameters, transfer table and precompiled shader projections.
#include <array>
#include <cmath>
#include <cstdint>

#include "donner/gpu/shader/CompiledShader.h"
namespace donner::gpu::shader::programs {
/// Uniform direction selector.
struct ColorSpaceConvertParams {
  uint32_t
      direction;  //!< \ref kColorSpaceConvertSrgbToLinear or \ref kColorSpaceConvertLinearToSrgb.
  uint32_t pad0;  //!< Reserved layout padding.
  uint32_t pad1;  //!< Reserved layout padding.
  uint32_t pad2;  //!< Reserved layout padding.
};
static_assert(sizeof(ColorSpaceConvertParams) == 16);
/// Direction value converting sRGB input to linear output.
inline constexpr uint32_t kColorSpaceConvertSrgbToLinear = 0;
/// Direction value converting linear input to sRGB output.
inline constexpr uint32_t kColorSpaceConvertLinearToSrgb = 1;
/// Samples per transfer direction; the table holds the forward then the inverse curve.
inline constexpr uint32_t kColorTransferSampleCount = 4096;
/// Read-only storage layout of the shared transfer table.
struct ColorTransferTable {
  float samples[2 * kColorTransferSampleCount];  //!< sRGB-to-linear then linear-to-sRGB.
};
static_assert(sizeof(ColorTransferTable) == 32768);
/// Returns the process-lifetime transfer samples in the shader's table order.
/// @return Stable reference to the shared table contents.
inline const std::array<float, 2 * kColorTransferSampleCount>& ColorTransferSamples() {
  static const std::array<float, 2 * kColorTransferSampleCount> samples = [] {
    std::array<float, 2 * kColorTransferSampleCount> result{};
    for (uint32_t i = 0; i < kColorTransferSampleCount; ++i) {
      const double c = static_cast<double>(i) / (kColorTransferSampleCount - 1);
      result[i] = static_cast<float>(c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4));
      result[kColorTransferSampleCount + i] =
          static_cast<float>(c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055);
    }
    return result;
  }();
  return samples;
}
/// Returns the WGSL color-space artifact: straight-alpha channels pass through the selected half of
/// the transfer table.
/// @return Stable view into process-lifetime data.
const CompiledShaderView& ColorSpaceConvertShader();
/// Returns only the platform-native ColorSpaceConvert projection.
/// @return Stable view into process-lifetime data.
const CompiledShaderView& ColorSpaceConvertNativeShader();
}  // namespace donner::gpu::shader::programs
