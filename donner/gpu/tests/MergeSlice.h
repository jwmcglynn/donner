#pragma once
/// @file
/// Native feMerge execution: one premultiplied source-over pass against an exact host reference.

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
namespace merge_slice {
inline constexpr uint32_t kWidth = 7, kHeight = 5, kRowBytes = 256;
using Texel = std::array<float, 4>;

/// Source texel: dyadic values keep every product and sum exact in f32, so the comparison below
/// can be bit-exact instead of tolerant. The last entry is deliberately not premultiplied (color
/// above alpha); valid premultiplied inputs can never exceed one under source-over, so it is the
/// only way to reach the shader's saturate.
inline Texel SourceTexel(uint32_t x, uint32_t y) {
  constexpr std::array<Texel, 6> kTexels{
      Texel{0.25f, 0.125f, 0.0625f, 0.5f},     Texel{0.0f, 0.0f, 0.0f, 0.0f},
      Texel{1.0f, 0.75f, 0.5f, 1.0f},          Texel{0.375f, 0.25f, 0.125f, 0.75f},
      Texel{0.03125f, 0.0625f, 0.125f, 0.25f}, Texel{0.75f, 0.5f, 0.25f, 0.25f}};
  return kTexels[(x + 2 * y) % kTexels.size()];
}

/// Premultiplied backdrop texel; the opaque white entry drives the out-of-range source past one.
inline Texel DestinationTexel(uint32_t x, uint32_t y) {
  constexpr std::array<Texel, 4> kTexels{
      Texel{0.5f, 0.25f, 0.75f, 1.0f}, Texel{0.125f, 0.5f, 0.25f, 0.5f},
      Texel{0.0f, 0.0f, 0.0f, 0.0f}, Texel{1.0f, 1.0f, 1.0f, 1.0f}};
  return kTexels[(3 * x + y) % kTexels.size()];
}

/// The shader's contract: source over destination, then saturate.
inline Texel Expected(uint32_t x, uint32_t y) {
  const Texel source = SourceTexel(x, y);
  const Texel destination = DestinationTexel(x, y);
  const float remaining = 1.0f - source[3];
  Texel result{};
  for (size_t c = 0; c < 4; ++c) {
    result[c] = std::clamp(source[c] + destination[c] * remaining, 0.0f, 1.0f);
  }
  return result;
}

inline std::vector<uint8_t> Upload(bool source) {
  std::vector<uint8_t> bytes(kRowBytes * kHeight);
  for (uint32_t y = 0; y < kHeight; ++y) {
    for (uint32_t x = 0; x < kWidth; ++x) {
      const Texel texel = source ? SourceTexel(x, y) : DestinationTexel(x, y);
      std::memcpy(bytes.data() + y * kRowBytes + x * sizeof(texel), texel.data(), sizeof(texel));
    }
  }
  return bytes;
}
}  // namespace merge_slice

/// Runs one merge pass through reflected bindings and compares every texel exactly.
/// @param device Native device. @param shader Selected or mutation artifact.
/// @param readbackBuffer Bounded backend readback.
template <class DeviceType, class Readback>
void CheckMerge(DeviceType& device, const shader::CompiledShaderView& shader,
                Readback readbackBuffer) {
  using namespace merge_slice;
  ReflectedComputePipeline compute;
  CreateReflectedComputePipeline(device, shader, "merge", compute);
  if (testing::Test::HasFatalFailure()) return;
  std::vector<Texture> textures;
  std::vector<TextureView> views;
  std::vector<BindGroupEntry> entries;
  for (bool source : {true, false}) {
    const char* name = source ? "sourceTexture" : "destinationTexture";
    auto texture = device.createTexture({name,
                                         {kWidth, kHeight},
                                         TextureFormat::RGBA32Float,
                                         TextureUsage::Sampled | TextureUsage::CopyDst});
    ASSERT_THAT(texture, HasResult());
    auto view = device.createTextureView(texture.result(), {name});
    ASSERT_THAT(view, HasResult());
    ASSERT_THAT(device.writeTexture(texture.result(), Upload(source), {0, kRowBytes, kHeight},
                                    {kWidth, kHeight}),
                IsOk());
    entries.push_back({ReflectedBinding(shader, name), TextureViewBinding{view.result()}});
    textures.push_back(std::move(texture).result());
    views.push_back(std::move(view).result());
  }
  auto output = device.createTexture({"merge output",
                                      {kWidth, kHeight},
                                      TextureFormat::RGBA32Float,
                                      TextureUsage::StorageBinding | TextureUsage::CopySrc});
  ASSERT_THAT(output, HasResult());
  auto outputView = device.createTextureView(output.result(), {"merge output"});
  ASSERT_THAT(outputView, HasResult());
  entries.push_back(
      {ReflectedBinding(shader, "outputTexture"), TextureViewBinding{outputView.result()}});
  auto group = device.createBindGroup({"merge", compute.layout, entries});
  ASSERT_THAT(group, HasResult());
  auto readback = device.createBuffer(
      {"merge readback", kRowBytes * kHeight, BufferUsage::CopyDst | BufferUsage::MapRead});
  ASSERT_THAT(readback, HasResult());
  auto encoder = device.createCommandEncoder();
  ASSERT_THAT(encoder, HasResult());
  auto pass = encoder.result()->beginComputePass({"merge"});
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
      EXPECT_THAT(actual, testing::ElementsAreArray(Expected(x, y))) << "pixel=" << x << "," << y;
    }
  }
}

}  // namespace donner::gpu::tests
