#pragma once
/// @file
/// Bind group indices of the sRGB-to-linear color space conversion program.
///
/// Split out from the program header so a host can build the program's bind group layout without
/// linking the shader IR. The pipeline that does so consumes build-time emitted WGSL, and pulling
/// in the IR for an enum would put the emitters back in binaries that exist to avoid them.

#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace donner::gpu::shader::programs {

/// Name of the compute entry point, as the IR declares it and the emitted source spells it.
inline constexpr std::string_view kColorSpaceConvertEntryPoint = "cs_main";

/// Workgroup size the entry point declares.
///
/// One definition, read by the IR builder that writes the shader and by every host that creates
/// the pipeline or sizes a dispatch from it. A host restating it could drift, and a larger stale
/// copy would silently under-dispatch, leaving the tail of the destination unwritten.
inline constexpr uint32_t kColorSpaceConvertWorkgroupSize = 8;

/// Bind group indices of the color space conversion program, shared by the IR builder and every
/// host that creates its bind group layout.
enum class ColorSpaceConvertBinding : uint32_t {
  InputTexture = 0,   //!< Sampled `texture_2d<f32>` source.
  OutputTexture = 1,  //!< `texture_storage_2d<rgba32float, write>` destination.
  TransferTable = 3,  //!< Read-only float transfer samples, both directions.
  Params = 2,         //!< Uniform buffer selecting the direction of the transfer.
};

/// Value of the direction parameter that converts sRGB-encoded channels to linear light.
inline constexpr uint32_t kColorSpaceConvertSrgbToLinear = 0;

/// Value of the direction parameter that converts linear-light channels to sRGB encoding.
inline constexpr uint32_t kColorSpaceConvertLinearToSrgb = 1;

/// Samples per direction, including both endpoints of the unit interval.
inline constexpr uint32_t kColorTransferSampleCount = 4096;

/// Returns process-lifetime samples of both standard transfer functions, rounded once to float.
/// The first half maps sRGB to linear light; the second half maps linear light to sRGB.
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

}  // namespace donner::gpu::shader::programs
