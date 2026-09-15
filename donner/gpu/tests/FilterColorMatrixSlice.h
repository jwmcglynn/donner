#pragma once
/// @file
/// Native feColorMatrix execution against a straight-alpha host reference.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/shader/programs/FilterColorMatrix.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/gpu/tests/ReflectedComputeSlice.h"
#include "tiny_skia/filter/FloatPixmap.h"

namespace donner::gpu::tests {
namespace filter_color_matrix_slice {
inline constexpr uint32_t kWidth = 7, kHeight = 5, kRowBytes = 256;
using Texel = std::array<float, 4>;

/// Matrix cases. Dyadic matrices are compared bit-exactly; the transcendental-looking SVG
/// presets go through the 8-bit pixel comparator because their sums round differently per backend.
enum class Case {
  Identity,
  Saturate,
  HueRotate,
  LuminanceToAlpha,
  OffsetOnly,
  ZeroAlphaOffset,
  Clamped
};

inline bool ExactCase(Case testCase) {
  return testCase == Case::Identity || testCase == Case::OffsetOnly ||
         testCase == Case::ZeroAlphaOffset || testCase == Case::Clamped;
}

/// Five columns as the shader reads them: col0..col3 weight R, G, B, A and col4 is the offset.
using Matrix = std::array<Texel, 5>;

inline Matrix MatrixFor(Case testCase) {
  Matrix m{};
  switch (testCase) {
    case Case::Identity:
      m = {Texel{1, 0, 0, 0}, Texel{0, 1, 0, 0}, Texel{0, 0, 1, 0}, Texel{0, 0, 0, 1},
           Texel{0, 0, 0, 0}};
      break;
    case Case::Saturate: {
      const float s = 0.5f;
      m = {Texel{0.213f + 0.787f * s, 0.213f - 0.213f * s, 0.213f - 0.213f * s, 0},
           Texel{0.715f - 0.715f * s, 0.715f + 0.285f * s, 0.715f - 0.715f * s, 0},
           Texel{0.072f - 0.072f * s, 0.072f - 0.072f * s, 0.072f + 0.928f * s, 0},
           Texel{0, 0, 0, 1}, Texel{0, 0, 0, 0}};
      break;
    }
    case Case::HueRotate:
      // The SVG hueRotate matrix at 90 degrees (cos 0, sin 1), stored by input column.
      m = {Texel{0.213f - 0.213f, 0.213f + 0.143f, 0.213f - 0.787f, 0},
           Texel{0.715f - 0.715f, 0.715f + 0.140f, 0.715f + 0.715f, 0},
           Texel{0.072f + 0.928f, 0.072f - 0.283f, 0.072f + 0.072f, 0}, Texel{0, 0, 0, 1},
           Texel{0, 0, 0, 0}};
      break;
    case Case::LuminanceToAlpha:
      m = {Texel{0, 0, 0, 0.2125f}, Texel{0, 0, 0, 0.7154f}, Texel{0, 0, 0, 0.0721f},
           Texel{0, 0, 0, 0}, Texel{0, 0, 0, 0}};
      break;
    case Case::OffsetOnly:
      m = {Texel{0, 0, 0, 0}, Texel{0, 0, 0, 0}, Texel{0, 0, 0, 0}, Texel{0, 0, 0, 0},
           Texel{0.25f, 0.5f, 0.75f, 0.5f}};
      break;
    case Case::ZeroAlphaOffset:
      // Alpha passes through; zero-alpha inputs receive the premultiplied offset color only when
      // the offset alpha is nonzero.
      m = {Texel{1, 0, 0, 0}, Texel{0, 1, 0, 0}, Texel{0, 0, 1, 0}, Texel{0, 0, 0, 1},
           Texel{0.5f, 0.25f, 0.125f, 0.5f}};
      break;
    case Case::Clamped:
      m = {Texel{2, 0, 0, 0}, Texel{0, -2, 0, 0}, Texel{0, 0, 4, 0}, Texel{0, 0, 0, 2},
           Texel{0, 0.5f, -0.5f, 0}};
      break;
  }
  return m;
}

/// Premultiplied input with dyadic components and power-of-two alphas, so demultiplying and
/// premultiplying again are exact; it includes zero-alpha and opaque texels and one texel whose
/// red exceeds its alpha, so the straight value above one reaches the clamp.
inline Texel InputTexel(uint32_t x, uint32_t y) {
  constexpr std::array<Texel, 7> kTexels{
      Texel{0.25f, 0.125f, 0.0625f, 0.5f},     Texel{0.0f, 0.0f, 0.0f, 0.0f},
      Texel{1.0f, 0.75f, 0.5f, 1.0f},          Texel{0.375f, 0.25f, 0.125f, 0.5f},
      Texel{0.03125f, 0.0625f, 0.125f, 0.25f}, Texel{0.5f, 0.5f, 0.5f, 0.5f},
      Texel{0.75f, 0.5f, 0.25f, 0.5f}};
  return kTexels[(x + 2 * y) % kTexels.size()];
}

/// Mirrors the shader: straight-alpha weighting, clamp, premultiply, with the zero-alpha branch.
inline Texel Expected(Case testCase, uint32_t x, uint32_t y) {
  const Matrix m = MatrixFor(testCase);
  const Texel source = InputTexel(x, y);
  Texel result{};
  if (source[3] > 0.0f) {
    const Texel straight{source[0] / source[3], source[1] / source[3], source[2] / source[3],
                         source[3]};
    for (size_t c = 0; c < 4; ++c) {
      const float weighted = m[0][c] * straight[0] + m[1][c] * straight[1] + m[2][c] * straight[2] +
                             m[3][c] * straight[3] + m[4][c];
      result[c] = std::clamp(weighted, 0.0f, 1.0f);
    }
    for (size_t c = 0; c < 3; ++c) result[c] *= result[3];
    return result;
  }
  Texel offset{};
  for (size_t c = 0; c < 4; ++c) offset[c] = std::clamp(m[4][c], 0.0f, 1.0f);
  if (offset[3] == 0.0f) return result;
  return {offset[0] * offset[3], offset[1] * offset[3], offset[2] * offset[3], offset[3]};
}

inline std::vector<uint8_t> Upload() {
  std::vector<uint8_t> bytes(kRowBytes * kHeight);
  for (uint32_t y = 0; y < kHeight; ++y) {
    for (uint32_t x = 0; x < kWidth; ++x) {
      const Texel texel = InputTexel(x, y);
      std::memcpy(bytes.data() + y * kRowBytes + x * sizeof(texel), texel.data(), sizeof(texel));
    }
  }
  return bytes;
}
}  // namespace filter_color_matrix_slice

/// Dispatches one matrix through reflected bindings and compares against the host reference.
/// @param device Native device. @param shader Selected or mutation artifact.
/// @param readbackBuffer Bounded backend readback. @param testCase Matrix under test.
template <class DeviceType, class Readback>
void CheckFilterColorMatrix(DeviceType& device, const shader::CompiledShaderView& shader,
                            Readback readbackBuffer, filter_color_matrix_slice::Case testCase) {
  using namespace filter_color_matrix_slice;
  ReflectedComputePipeline compute;
  CreateReflectedComputePipeline(device, shader, "color matrix", compute);
  if (testing::Test::HasFatalFailure()) return;
  auto input = device.createTexture({"color matrix input",
                                     {kWidth, kHeight},
                                     TextureFormat::RGBA32Float,
                                     TextureUsage::Sampled | TextureUsage::CopyDst});
  ASSERT_THAT(input, HasResult());
  auto inputView = device.createTextureView(input.result(), {"color matrix input"});
  ASSERT_THAT(inputView, HasResult());
  ASSERT_THAT(
      device.writeTexture(input.result(), Upload(), {0, kRowBytes, kHeight}, {kWidth, kHeight}),
      IsOk());
  auto output = device.createTexture({"color matrix output",
                                      {kWidth, kHeight},
                                      TextureFormat::RGBA32Float,
                                      TextureUsage::StorageBinding | TextureUsage::CopySrc});
  ASSERT_THAT(output, HasResult());
  auto outputView = device.createTextureView(output.result(), {"color matrix output"});
  ASSERT_THAT(outputView, HasResult());
  const Matrix matrix = MatrixFor(testCase);
  shader::programs::FilterColorMatrixParams params{};
  std::memcpy(params.col0, matrix[0].data(), sizeof(params.col0));
  std::memcpy(params.col1, matrix[1].data(), sizeof(params.col1));
  std::memcpy(params.col2, matrix[2].data(), sizeof(params.col2));
  std::memcpy(params.col3, matrix[3].data(), sizeof(params.col3));
  std::memcpy(params.col4, matrix[4].data(), sizeof(params.col4));
  auto uniform = device.createBuffer(
      {"color matrix parameters", sizeof(params), BufferUsage::Uniform | BufferUsage::CopyDst});
  ASSERT_THAT(uniform, HasResult());
  ASSERT_THAT(device.writeBuffer(uniform.result(), 0,
                                 {reinterpret_cast<const uint8_t*>(&params), sizeof(params)}),
              IsOk());
  auto group = device.createBindGroup(
      {"color matrix",
       compute.layout,
       {{ReflectedBinding(shader, "inputTexture"), TextureViewBinding{inputView.result()}},
        {ReflectedBinding(shader, "outputTexture"), TextureViewBinding{outputView.result()}},
        {ReflectedBinding(shader, "params"), BufferBinding{uniform.result(), 0, sizeof(params)}}}});
  ASSERT_THAT(group, HasResult());
  auto readback = device.createBuffer(
      {"color matrix readback", kRowBytes * kHeight, BufferUsage::CopyDst | BufferUsage::MapRead});
  ASSERT_THAT(readback, HasResult());
  auto encoder = device.createCommandEncoder();
  ASSERT_THAT(encoder, HasResult());
  auto pass = encoder.result()->beginComputePass({"color matrix"});
  ASSERT_THAT(pass, HasResult());
  ASSERT_THAT(pass.result()->setPipeline(compute.pipeline), IsOk());
  ASSERT_THAT(pass.result()->setBindGroup(0, group.result()), IsOk());
  const auto groups = compute.groupsFor(kWidth, kHeight);
  ASSERT_THAT(pass.result()->dispatchWorkgroups(groups[0], groups[1], groups[2]), IsOk());
  ASSERT_THAT(pass.result()->end(), IsOk());
  ASSERT_THAT(encoder.result()->copyTextureToBuffer({output.result()}, readback.result(),
                                                    {0, kRowBytes, kHeight}, {kWidth, kHeight}),
              IsOk());
  auto commands = encoder.result()->finish();
  ASSERT_THAT(commands, HasResult());
  auto serial = device.submit(std::move(commands).result());
  ASSERT_THAT(serial, HasResult());
  ASSERT_THAT(device.waitForSerial(serial.result(), 5.0), testing::IsTrue());
  const auto bytes = readbackBuffer(readback.result());
  ASSERT_THAT(bytes, HasResult());
  ASSERT_THAT(bytes.result(), testing::SizeIs(testing::Ge(kRowBytes * kHeight)));
  auto actual = tiny_skia::filter::FloatPixmap::fromSize(kWidth, kHeight).value();
  auto expected = tiny_skia::filter::FloatPixmap::fromSize(kWidth, kHeight).value();
  for (uint32_t y = 0; y < kHeight; ++y) {
    std::memcpy(actual.data().data() + y * kWidth * 4, bytes.result().data() + y * kRowBytes,
                kWidth * 4 * sizeof(float));
    for (uint32_t x = 0; x < kWidth; ++x) {
      const Texel texel = Expected(testCase, x, y);
      std::copy(texel.begin(), texel.end(), expected.data().begin() + (y * kWidth + x) * 4);
    }
  }
  if (ExactCase(testCase)) {
    for (uint32_t y = 0; y < kHeight; ++y) {
      for (uint32_t x = 0; x < kWidth; ++x) {
        Texel got{}, want{};
        std::copy_n(actual.data().begin() + (y * kWidth + x) * 4, 4, got.begin());
        std::copy_n(expected.data().begin() + (y * kWidth + x) * 4, 4, want.begin());
        EXPECT_THAT(got, testing::ElementsAreArray(want)) << "pixel=" << x << "," << y;
      }
    }
    return;
  }
  const auto actualPixels = actual.toPixmap();
  const auto expectedPixels = expected.toPixmap();
  editor::tests::CompareBitmapToBitmap(
      svg::RendererBitmap{Vector2i(kWidth, kHeight),
                          {actualPixels.data().begin(), actualPixels.data().end()},
                          kWidth * 4},
      svg::RendererBitmap{Vector2i(kWidth, kHeight),
                          {expectedPixels.data().begin(), expectedPixels.data().end()},
                          kWidth * 4},
      "filter_color_matrix_" + std::to_string(static_cast<uint32_t>(testCase)) + "_" +
          std::string(shader.entryPoints.front().name.view()),
      editor::tests::PixelmatchIdentityParams());
}

}  // namespace donner::gpu::tests
