#pragma once
/// @file
/// Native snapshot unpremultiply: premultiplied RGBA8 to straight RGBA8 with the host's rounding.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/gpu/tests/ReflectedComputeSlice.h"

namespace donner::gpu::tests {
namespace snapshot_unpremultiply_slice {
inline constexpr uint32_t kWidth = 7, kHeight = 5, kRowBytes = 256;
using Pixel = std::array<uint8_t, 4>;

/// Premultiplied input bytes: zero alpha, full alpha, awkward divisors, and one texel whose color
/// exceeds its alpha so the saturating minimum has to fire.
inline Pixel InputPixel(uint32_t x, uint32_t y) {
  constexpr std::array<Pixel, 7> kPixels{
      Pixel{0, 0, 0, 0}, Pixel{255, 128, 0, 255},   Pixel{100, 50, 25, 100}, Pixel{64, 32, 16, 128},
      Pixel{3, 2, 1, 7}, Pixel{200, 150, 100, 100}, Pixel{1, 1, 1, 255}};
  return kPixels[(x + 3 * y) % kPixels.size()];
}

/// The host readback formula: round half up at eight bits, saturating overflows.
inline Pixel Expected(uint32_t x, uint32_t y) {
  const Pixel input = InputPixel(x, y);
  const uint32_t alpha = input[3];
  if (alpha == 0) return {0, 0, 0, 0};
  const uint32_t half = alpha / 2;
  Pixel result{};
  for (size_t c = 0; c < 3; ++c) {
    result[c] = static_cast<uint8_t>(std::min(255u, (input[c] * 255u + half) / alpha));
  }
  result[3] = input[3];
  return result;
}

inline std::vector<uint8_t> Upload() {
  std::vector<uint8_t> bytes(kRowBytes * kHeight);
  for (uint32_t y = 0; y < kHeight; ++y) {
    for (uint32_t x = 0; x < kWidth; ++x) {
      const Pixel pixel = InputPixel(x, y);
      std::memcpy(bytes.data() + y * kRowBytes + x * 4, pixel.data(), 4);
    }
  }
  return bytes;
}
}  // namespace snapshot_unpremultiply_slice

/// Dispatches the unpremultiply pass through reflected bindings and compares every byte.
/// @param device Native device. @param shader Selected or mutation artifact.
/// @param readbackBuffer Bounded backend readback.
template <class DeviceType, class Readback>
void CheckSnapshotUnpremultiply(DeviceType& device, const shader::CompiledShaderView& shader,
                                Readback readbackBuffer) {
  using namespace snapshot_unpremultiply_slice;
  ReflectedComputePipeline compute;
  CreateReflectedComputePipeline(device, shader, "snapshot unpremultiply", compute);
  if (testing::Test::HasFatalFailure()) return;
  auto input = device.createTexture({"snapshot input",
                                     {kWidth, kHeight},
                                     TextureFormat::RGBA8Unorm,
                                     TextureUsage::Sampled | TextureUsage::CopyDst});
  ASSERT_THAT(input, HasResult());
  auto inputView = device.createTextureView(input.result(), {"snapshot input"});
  ASSERT_THAT(inputView, HasResult());
  ASSERT_THAT(
      device.writeTexture(input.result(), Upload(), {0, kRowBytes, kHeight}, {kWidth, kHeight}),
      IsOk());
  auto output = device.createTexture({"snapshot output",
                                      {kWidth, kHeight},
                                      TextureFormat::RGBA8Unorm,
                                      TextureUsage::StorageBinding | TextureUsage::CopySrc});
  ASSERT_THAT(output, HasResult());
  auto outputView = device.createTextureView(output.result(), {"snapshot output"});
  ASSERT_THAT(outputView, HasResult());
  auto group = device.createBindGroup(
      {"snapshot unpremultiply",
       compute.layout,
       {{ReflectedBinding(shader, "inputTexture"), TextureViewBinding{inputView.result()}},
        {ReflectedBinding(shader, "outputTexture"), TextureViewBinding{outputView.result()}}}});
  ASSERT_THAT(group, HasResult());
  auto readback = device.createBuffer(
      {"snapshot readback", kRowBytes * kHeight, BufferUsage::CopyDst | BufferUsage::MapRead});
  ASSERT_THAT(readback, HasResult());
  auto encoder = device.createCommandEncoder();
  ASSERT_THAT(encoder, HasResult());
  auto pass = encoder.result()->beginComputePass({"snapshot unpremultiply"});
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
      Pixel actual{};
      std::memcpy(actual.data(), bytes.result().data() + y * kRowBytes + x * 4, 4);
      EXPECT_THAT(actual, testing::ElementsAreArray(Expected(x, y))) << "pixel=" << x << "," << y;
    }
  }
}

}  // namespace donner::gpu::tests
