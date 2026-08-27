#pragma once
/// @file
/// Shared scene for the color-matrix compute vertical slices.
///
/// Every backend slice runs the same kernel over the same input and checks the same expected
/// bytes, so a backend-specific divergence shows up as a byte difference rather than as two
/// tests that happen to agree with themselves.
///
/// The numbers are chosen so the whole computation is exact in 8-bit unorm. Input channels are
/// multiples of 8, matrix coefficients are 0.5 and 0.25, and the bias is a whole multiple of
/// 1/255, so every result lands on an integer texel value with no rounding freedom: the slices
/// compare bytes exactly rather than within a quantization tolerance.

#include <array>
#include <cstdint>
#include <vector>

namespace donner::gpu::tests {

/// Destination width in texels. Deliberately not a multiple of the workgroup size so the
/// kernel's bounds guard is exercised by a dispatch rounded up to whole workgroups.
inline constexpr uint32_t kColorMatrixWidth = 20;

/// Destination height in texels; also not a multiple of the workgroup size.
inline constexpr uint32_t kColorMatrixHeight = 12;

/// Bytes per row of the readback staging buffer, meeting the runtime's 256-byte row pitch.
inline constexpr uint32_t kColorMatrixBytesPerRow = 256;

/// Bias added to the red channel, in texel units, for invocations where x > y.
inline constexpr uint32_t kColorMatrixRedBias = 8;

/// Blue value of every input texel, in texel units.
inline constexpr uint32_t kColorMatrixInputBlue = 64;

/// Alpha value of every input texel, in texel units.
inline constexpr uint32_t kColorMatrixInputAlpha = 248;

/// The 80-byte uniform block: four `vec4<f32>` columns of per-source-channel multipliers, then
/// the constant column added after them. An SVG color matrix is four rows of five, and the last
/// column shifts each output channel regardless of the input.
struct alignas(16) ColorMatrixParams {
  float row0[4];  //!< Contribution of the source red channel.
  float row1[4];  //!< Contribution of the source green channel.
  float row2[4];  //!< Contribution of the source blue channel.
  float row3[4];  //!< Contribution of the source alpha channel.
  float row4[4];  //!< Constant offset added after the weighted sum.
};
static_assert(sizeof(ColorMatrixParams) == 80, "ColorMatrixParams must match the shader layout");

/// The matrix the slices upload: red and green are halved and gain a quarter of blue, blue and
/// alpha are halved.
inline ColorMatrixParams ColorMatrixUniforms() {
  return ColorMatrixParams{{0.5f, 0.0f, 0.0f, 0.0f},
                           {0.0f, 0.5f, 0.0f, 0.0f},
                           {0.25f, 0.25f, 0.5f, 0.0f},
                           {0.0f, 0.0f, 0.0f, 0.5f},
                           // Zero offset: this matrix shifts nothing, so the expected pixels are
                           // the same ones the four-column form produced.
                           {0.0f, 0.0f, 0.0f, 0.0f}};
}

/// The two bias vectors the kernel selects between: none, then a red-only offset.
inline std::array<float, 8> ColorMatrixBias() {
  const float redBias = static_cast<float>(kColorMatrixRedBias) / 255.0f;
  return {0.0f, 0.0f, 0.0f, 0.0f, redBias, 0.0f, 0.0f, 0.0f};
}

/// Builds the input texture contents: tightly packed RGBA8 rows of
/// `kColorMatrixWidth * kColorMatrixHeight` texels.
inline std::vector<uint8_t> ColorMatrixInputTexels() {
  std::vector<uint8_t> texels(size_t{kColorMatrixWidth} * kColorMatrixHeight * 4u);
  for (uint32_t y = 0; y < kColorMatrixHeight; ++y) {
    for (uint32_t x = 0; x < kColorMatrixWidth; ++x) {
      const size_t offset = (size_t{y} * kColorMatrixWidth + x) * 4u;
      texels[offset + 0] = static_cast<uint8_t>(8u * x);
      texels[offset + 1] = static_cast<uint8_t>(8u * y);
      texels[offset + 2] = static_cast<uint8_t>(kColorMatrixInputBlue);
      texels[offset + 3] = static_cast<uint8_t>(kColorMatrixInputAlpha);
    }
  }
  return texels;
}

/// The expected destination texel at (\p x, \p y), computed on the host from the same matrix.
/// @param x Texel column. @param y Texel row.
inline std::array<uint8_t, 4> ColorMatrixExpectedTexel(uint32_t x, uint32_t y) {
  const uint32_t red = 4u * x + kColorMatrixInputBlue / 4u + (x > y ? kColorMatrixRedBias : 0u);
  const uint32_t green = 4u * y + kColorMatrixInputBlue / 4u;
  const uint32_t blue = kColorMatrixInputBlue / 2u;
  const uint32_t alpha = kColorMatrixInputAlpha / 2u;
  return {static_cast<uint8_t>(red), static_cast<uint8_t>(green), static_cast<uint8_t>(blue),
          static_cast<uint8_t>(alpha)};
}

/// Number of workgroups a dispatch needs along one axis to cover \p extent texels.
/// @param extent Destination extent in texels.
/// @param workgroupSize Invocations per workgroup along the same axis.
inline uint32_t ColorMatrixWorkgroupCount(uint32_t extent, uint32_t workgroupSize) {
  return (extent + workgroupSize - 1u) / workgroupSize;
}

}  // namespace donner::gpu::tests
