#pragma once
/// @file
/// Native subregion clipping: pixel centers mapped back to user space against a half-open
/// rectangle.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/shader/programs/SubregionClip.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/gpu/tests/ReflectedComputeSlice.h"

namespace donner::gpu::tests {
namespace subregion_clip_slice {
inline constexpr uint32_t kWidth = 9, kHeight = 7, kRowBytes = 256;
using Texel = std::array<float, 4>;

/// Transform cases. The identity rectangle puts pixel centers exactly on all four edges, which
/// pins the half-open contract (low edges inclusive, high edges exclusive); the other cases keep
/// their edges away from any mapped center.
enum class Case { Identity, Scaled, Rotated, Empty };

inline shader::programs::SubregionClipParams ParamsFor(Case testCase) {
  shader::programs::SubregionClipParams params{};
  switch (testCase) {
    case Case::Identity:
      params.invA = 1;
      params.invD = 1;
      params.userX0 = 2.5f;
      params.userY0 = 1.5f;
      params.userX1 = 6.5f;
      params.userY1 = 5.5f;
      break;
    case Case::Scaled:
      // Device pixels are twice the user size, so the inverse halves the pixel center.
      params.invA = 0.5f;
      params.invD = 0.5f;
      params.userX0 = 1;
      params.userY0 = 1;
      params.userX1 = 3;
      params.userY1 = 3;
      break;
    case Case::Rotated:
      // A quarter turn: user x follows device y and user y follows negative device x.
      params.invC = 1;
      params.invB = -1;
      params.invF = 9;
      params.userX0 = 1;
      params.userY0 = 2;
      params.userX1 = 4;
      params.userY1 = 6;
      break;
    case Case::Empty:
      params.invA = 1;
      params.invD = 1;
      params.userX0 = 4;
      params.userY0 = 1;
      params.userX1 = 4;
      params.userY1 = 5;
      break;
  }
  return params;
}

/// Distinct premultiplied texels so a shifted copy cannot masquerade as a correct one.
inline Texel InputTexel(uint32_t x, uint32_t y) {
  return {static_cast<float>(x + 1) / 16.0f, static_cast<float>(y + 1) / 8.0f,
          static_cast<float>((x * 3 + y) % 7) / 8.0f, 0.75f};
}

/// The shader's decision for one pixel center, with the same half-open comparisons.
inline bool Inside(const shader::programs::SubregionClipParams& p, uint32_t x, uint32_t y) {
  const float centerX = static_cast<float>(x) + 0.5f;
  const float centerY = static_cast<float>(y) + 0.5f;
  const float userX = p.invA * centerX + p.invC * centerY + p.invE;
  const float userY = p.invB * centerX + p.invD * centerY + p.invF;
  return !(userX < p.userX0 || userX >= p.userX1 || userY < p.userY0 || userY >= p.userY1);
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
}  // namespace subregion_clip_slice

/// Dispatches one clip through reflected bindings; kept texels copy exactly, others become zero.
/// @param device Native device. @param shader Selected or mutation artifact.
/// @param readbackBuffer Bounded backend readback. @param testCase Transform under test.
template <class DeviceType, class Readback>
void CheckSubregionClip(DeviceType& device, const shader::CompiledShaderView& shader,
                        Readback readbackBuffer, subregion_clip_slice::Case testCase) {
  using namespace subregion_clip_slice;
  ReflectedComputePipeline compute;
  CreateReflectedComputePipeline(device, shader, "subregion clip", compute);
  if (testing::Test::HasFatalFailure()) {
    return;
  }
  auto input = device.createTexture({"clip input",
                                     {kWidth, kHeight},
                                     TextureFormat::RGBA32Float,
                                     TextureUsage::Sampled | TextureUsage::CopyDst});
  ASSERT_THAT(input, HasResult());
  auto inputView = device.createTextureView(input.result(), {"clip input"});
  ASSERT_THAT(inputView, HasResult());
  ASSERT_THAT(
      device.writeTexture(input.result(), Upload(), {0, kRowBytes, kHeight}, {kWidth, kHeight}),
      IsOk());
  auto output = device.createTexture({"clip output",
                                      {kWidth, kHeight},
                                      TextureFormat::RGBA32Float,
                                      TextureUsage::StorageBinding | TextureUsage::CopySrc});
  ASSERT_THAT(output, HasResult());
  auto outputView = device.createTextureView(output.result(), {"clip output"});
  ASSERT_THAT(outputView, HasResult());
  const shader::programs::SubregionClipParams params = ParamsFor(testCase);
  auto uniform = device.createBuffer(
      {"clip parameters", sizeof(params), BufferUsage::Uniform | BufferUsage::CopyDst});
  ASSERT_THAT(uniform, HasResult());
  ASSERT_THAT(device.writeBuffer(uniform.result(), 0,
                                 {reinterpret_cast<const uint8_t*>(&params), sizeof(params)}),
              IsOk());
  auto group = device.createBindGroup(
      {"subregion clip",
       compute.layout,
       {{ReflectedBinding(shader, "inputTexture"), TextureViewBinding{inputView.result()}},
        {ReflectedBinding(shader, "outputTexture"), TextureViewBinding{outputView.result()}},
        {ReflectedBinding(shader, "params"), BufferBinding{uniform.result(), 0, sizeof(params)}}}});
  ASSERT_THAT(group, HasResult());
  auto readback = device.createBuffer(
      {"clip readback", kRowBytes * kHeight, BufferUsage::CopyDst | BufferUsage::MapRead});
  ASSERT_THAT(readback, HasResult());
  auto encoder = device.createCommandEncoder();
  ASSERT_THAT(encoder, HasResult());
  auto pass = encoder.result()->beginComputePass({"subregion clip"});
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
  uint32_t kept = 0;
  for (uint32_t y = 0; y < kHeight; ++y) {
    for (uint32_t x = 0; x < kWidth; ++x) {
      Texel actual{};
      std::memcpy(actual.data(), bytes.result().data() + y * kRowBytes + x * sizeof(actual),
                  sizeof(actual));
      const bool inside = Inside(params, x, y);
      kept += inside ? 1 : 0;
      const Texel expected = inside ? InputTexel(x, y) : Texel{0, 0, 0, 0};
      EXPECT_THAT(actual, testing::ElementsAreArray(expected))
          << "pixel=" << x << "," << y << " inside=" << inside;
    }
  }
  // Every case except the empty rectangle keeps a proper subset, so a shader that ignored the
  // rectangle in either direction cannot pass.
  if (testCase == Case::Empty) {
    EXPECT_EQ(kept, 0u);
  } else {
    EXPECT_GT(kept, 0u);
    EXPECT_LT(kept, kWidth * kHeight);
  }
}

}  // namespace donner::gpu::tests
