#pragma once
/// @file
/// Native color-space conversion through the shared transfer table, compared exactly.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/shader/programs/ColorSpaceConvert.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/gpu/tests/ReflectedComputeSlice.h"

namespace donner::gpu::tests {
namespace color_space_convert_slice {
inline constexpr uint32_t kWidth = 7, kHeight = 5, kRowBytes = 256;
using Texel = std::array<float, 4>;

/// Premultiplied input with power-of-two alphas, so demultiplying and re-multiplying are exact
/// and the table index is the only rounding the shader performs. Two texels carry channels above
/// their alpha to exercise the clamp to the last table entry.
inline Texel InputTexel(uint32_t x, uint32_t y) {
  constexpr std::array<Texel, 7> kTexels{
      Texel{0.25f, 0.125f, 0.0625f, 0.5f}, Texel{0.0f, 0.0f, 0.0f, 0.0f},
      Texel{1.0f, 0.75f, 0.5f, 1.0f},      Texel{0.1875f, 0.0f, 0.25f, 0.25f},
      Texel{0.75f, 0.5f, 0.25f, 0.5f},     Texel{0.03125f, 0.015625f, 0.5f, 1.0f},
      Texel{0.375f, 0.125f, 0.25f, 0.25f}};
  return kTexels[(x + 2 * y) % kTexels.size()];
}

/// The shader's exact lookup: clamp the straight channel to one, scale to the table, round half
/// up, read the selected half, and premultiply again.
inline Texel Expected(uint32_t x, uint32_t y, bool srgbToLinear) {
  const auto& table = shader::programs::ColorTransferSamples();
  const uint32_t base = srgbToLinear ? 0u : shader::programs::kColorTransferSampleCount;
  const Texel source = InputTexel(x, y);
  Texel straight{};
  if (source[3] > 0.0f) {
    const float inverse = 1.0f / source[3];
    straight = {source[0] * inverse, source[1] * inverse, source[2] * inverse, source[3]};
  }
  Texel result{};
  for (size_t c = 0; c < 3; ++c) {
    const float unit = straight[c] > 0.0f ? std::min(straight[c], 1.0f) : 0.0f;
    const float scaled = unit * 4095.0f;
    const uint32_t index = static_cast<uint32_t>(scaled + 0.5f);
    result[c] = std::clamp(table[base + index] * straight[3], 0.0f, 1.0f);
  }
  result[3] = std::clamp(straight[3], 0.0f, 1.0f);
  return result;
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
}  // namespace color_space_convert_slice

/// Dispatches one direction through reflected bindings and the shared table; compares exactly.
/// @param device Native device. @param shader Selected or mutation artifact.
/// @param readbackBuffer Bounded backend readback. @param srgbToLinear Direction under test.
template <class DeviceType, class Readback>
void CheckColorSpaceConvert(DeviceType& device, const shader::CompiledShaderView& shader,
                            Readback readbackBuffer, bool srgbToLinear) {
  using namespace color_space_convert_slice;
  ReflectedComputePipeline compute;
  CreateReflectedComputePipeline(device, shader, "color space convert", compute);
  if (testing::Test::HasFatalFailure()) return;
  auto input = device.createTexture({"convert input",
                                     {kWidth, kHeight},
                                     TextureFormat::RGBA32Float,
                                     TextureUsage::Sampled | TextureUsage::CopyDst});
  ASSERT_THAT(input, HasResult());
  auto inputView = device.createTextureView(input.result(), {"convert input"});
  ASSERT_THAT(inputView, HasResult());
  ASSERT_THAT(
      device.writeTexture(input.result(), Upload(), {0, kRowBytes, kHeight}, {kWidth, kHeight}),
      IsOk());
  auto output = device.createTexture({"convert output",
                                      {kWidth, kHeight},
                                      TextureFormat::RGBA32Float,
                                      TextureUsage::StorageBinding | TextureUsage::CopySrc});
  ASSERT_THAT(output, HasResult());
  auto outputView = device.createTextureView(output.result(), {"convert output"});
  ASSERT_THAT(outputView, HasResult());
  const shader::programs::ColorSpaceConvertParams params{
      srgbToLinear ? shader::programs::kColorSpaceConvertSrgbToLinear
                   : shader::programs::kColorSpaceConvertLinearToSrgb,
      0, 0, 0};
  auto uniform = device.createBuffer(
      {"convert parameters", sizeof(params), BufferUsage::Uniform | BufferUsage::CopyDst});
  ASSERT_THAT(uniform, HasResult());
  ASSERT_THAT(device.writeBuffer(uniform.result(), 0,
                                 {reinterpret_cast<const uint8_t*>(&params), sizeof(params)}),
              IsOk());
  const auto& samples = shader::programs::ColorTransferSamples();
  auto table = device.createBuffer(
      {"transfer table", sizeof(samples), BufferUsage::Storage | BufferUsage::CopyDst});
  ASSERT_THAT(table, HasResult());
  ASSERT_THAT(
      device.writeBuffer(table.result(), 0,
                         {reinterpret_cast<const uint8_t*>(samples.data()), sizeof(samples)}),
      IsOk());
  auto group = device.createBindGroup(
      {"color space convert",
       compute.layout,
       {{ReflectedBinding(shader, "inputTexture"), TextureViewBinding{inputView.result()}},
        {ReflectedBinding(shader, "outputTexture"), TextureViewBinding{outputView.result()}},
        {ReflectedBinding(shader, "params"), BufferBinding{uniform.result(), 0, sizeof(params)}},
        {ReflectedBinding(shader, "transferTable"),
         BufferBinding{table.result(), 0, sizeof(samples)}}}});
  ASSERT_THAT(group, HasResult());
  auto readback = device.createBuffer(
      {"convert readback", kRowBytes * kHeight, BufferUsage::CopyDst | BufferUsage::MapRead});
  ASSERT_THAT(readback, HasResult());
  auto encoder = device.createCommandEncoder();
  ASSERT_THAT(encoder, HasResult());
  auto pass = encoder.result()->beginComputePass({"color space convert"});
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
  for (uint32_t y = 0; y < kHeight; ++y) {
    for (uint32_t x = 0; x < kWidth; ++x) {
      Texel actual{};
      std::memcpy(actual.data(), bytes.result().data() + y * kRowBytes + x * sizeof(actual),
                  sizeof(actual));
      EXPECT_THAT(actual, testing::ElementsAreArray(Expected(x, y, srgbToLinear)))
          << "srgbToLinear=" << srgbToLinear << " pixel=" << x << "," << y;
    }
  }
}

}  // namespace donner::gpu::tests
