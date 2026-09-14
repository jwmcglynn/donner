#pragma once
/// @file
/// Native feComposite execution: every operator against an exact premultiplied host reference.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/shader/programs/Composite.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/gpu/tests/ReflectedComputeSlice.h"

namespace donner::gpu::tests {
namespace composite_slice {
inline constexpr uint32_t kWidth = 7, kHeight = 5, kRowBytes = 256;
using Texel = std::array<float, 4>;

/// One operator case; UnknownOperator sends an index the shader has no branch for.
enum class Case { Over, In, Out, Atop, Xor, Lighter, Arithmetic, UnknownOperator };

inline uint32_t OperatorIndex(Case testCase) {
  switch (testCase) {
    case Case::Over: return 0;
    case Case::In: return 1;
    case Case::Out: return 2;
    case Case::Atop: return 3;
    case Case::Xor: return 4;
    case Case::Lighter: return 5;
    case Case::Arithmetic: return 6;
    case Case::UnknownOperator: return 9;
  }
  return 0;
}

/// Arithmetic coefficients; dyadic so the products stay exact, and chosen so opaque inputs exceed
/// one and the negative source weight drives dark inputs below zero, reaching both clamp edges.
inline constexpr std::array<float, 4> kArithmetic{1.0f, -1.0f, 1.0f, 0.5f};

/// Premultiplied source texel with dyadic components.
inline Texel SourceTexel(uint32_t x, uint32_t y) {
  constexpr std::array<Texel, 5> kTexels{
      Texel{0.25f, 0.125f, 0.0625f, 0.5f}, Texel{0.0f, 0.0f, 0.0f, 0.0f},
      Texel{1.0f, 0.75f, 0.5f, 1.0f}, Texel{0.375f, 0.25f, 0.125f, 0.75f},
      Texel{0.03125f, 0.0625f, 0.125f, 0.25f}};
  return kTexels[(x + 2 * y) % kTexels.size()];
}

/// Premultiplied destination texel with dyadic components.
inline Texel DestinationTexel(uint32_t x, uint32_t y) {
  constexpr std::array<Texel, 4> kTexels{
      Texel{0.5f, 0.25f, 0.75f, 1.0f}, Texel{0.125f, 0.5f, 0.25f, 0.5f},
      Texel{0.0f, 0.0f, 0.0f, 0.0f}, Texel{1.0f, 1.0f, 1.0f, 1.0f}};
  return kTexels[(3 * x + y) % kTexels.size()];
}

/// Porter-Duff and arithmetic references, saturated like the shader.
inline Texel Expected(Case testCase, uint32_t x, uint32_t y) {
  const Texel s = SourceTexel(x, y);
  const Texel d = DestinationTexel(x, y);
  const float remainingSource = 1.0f - s[3];
  const float remainingDestination = 1.0f - d[3];
  Texel result{};
  for (size_t c = 0; c < 4; ++c) {
    float value = s[c] + d[c] * remainingSource;
    switch (testCase) {
      case Case::Over:
      case Case::UnknownOperator: break;
      case Case::In: value = s[c] * d[3]; break;
      case Case::Out: value = s[c] * remainingDestination; break;
      case Case::Atop: value = s[c] * d[3] + d[c] * remainingSource; break;
      case Case::Xor: value = s[c] * remainingDestination + d[c] * remainingSource; break;
      case Case::Lighter: value = s[c] + d[c]; break;
      case Case::Arithmetic:
        value = kArithmetic[0] * s[c] * d[c] + kArithmetic[1] * s[c] + kArithmetic[2] * d[c] +
                kArithmetic[3];
        break;
    }
    result[c] = std::clamp(value, 0.0f, 1.0f);
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
}  // namespace composite_slice

/// Dispatches one operator through reflected bindings and compares every texel exactly.
/// @param device Native device. @param shader Selected or mutation artifact.
/// @param readbackBuffer Bounded backend readback. @param testCase Operator under test.
template <class DeviceType, class Readback>
void CheckComposite(DeviceType& device, const shader::CompiledShaderView& shader,
                    Readback readbackBuffer, composite_slice::Case testCase) {
  using namespace composite_slice;
  ReflectedComputePipeline compute;
  CreateReflectedComputePipeline(device, shader, "composite", compute);
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
  auto output = device.createTexture({"composite output",
                                      {kWidth, kHeight},
                                      TextureFormat::RGBA32Float,
                                      TextureUsage::StorageBinding | TextureUsage::CopySrc});
  ASSERT_THAT(output, HasResult());
  auto outputView = device.createTextureView(output.result(), {"composite output"});
  ASSERT_THAT(outputView, HasResult());
  entries.push_back(
      {ReflectedBinding(shader, "outputTexture"), TextureViewBinding{outputView.result()}});
  const shader::programs::CompositeParams params{
      OperatorIndex(testCase), 0, 0, 0, kArithmetic[0], kArithmetic[1], kArithmetic[2],
      kArithmetic[3]};
  auto uniform = device.createBuffer(
      {"composite parameters", sizeof(params), BufferUsage::Uniform | BufferUsage::CopyDst});
  ASSERT_THAT(uniform, HasResult());
  ASSERT_THAT(device.writeBuffer(uniform.result(), 0,
                                 {reinterpret_cast<const uint8_t*>(&params), sizeof(params)}),
              IsOk());
  entries.push_back(
      {ReflectedBinding(shader, "params"), BufferBinding{uniform.result(), 0, sizeof(params)}});
  auto group = device.createBindGroup({"composite", compute.layout, entries});
  ASSERT_THAT(group, HasResult());
  auto readback = device.createBuffer(
      {"composite readback", kRowBytes * kHeight, BufferUsage::CopyDst | BufferUsage::MapRead});
  ASSERT_THAT(readback, HasResult());
  auto encoder = device.createCommandEncoder();
  ASSERT_THAT(encoder, HasResult());
  auto pass = encoder.result()->beginComputePass({"composite"});
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
      EXPECT_THAT(actual, testing::ElementsAreArray(Expected(testCase, x, y)))
          << "operator=" << OperatorIndex(testCase) << " pixel=" << x << "," << y;
    }
  }
}

}  // namespace donner::gpu::tests
