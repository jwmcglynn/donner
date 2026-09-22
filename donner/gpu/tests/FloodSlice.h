#pragma once
/// @file
/// Native feFlood execution: every texel of an unaligned extent receives the uniform color.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/shader/programs/Flood.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/gpu/tests/ReflectedComputeSlice.h"

namespace donner::gpu::tests {
namespace flood_slice {
inline constexpr uint32_t kWidth = 13, kHeight = 5, kRowBytes = 256;
inline constexpr std::array<float, 4> kColor{0.125f, 0.25f, 0.375f, 0.5f};
}  // namespace flood_slice

/// Floods a 13x5 float texture and checks every texel bit-exactly. The extent is not a multiple of
/// either workgroup shape, so the edge invocations must return without writing out of bounds while
/// the in-range ones still cover the whole destination.
/// @param device Native device. @param shader Selected or mutation artifact.
/// @param readbackBuffer Bounded backend readback.
template <class DeviceType, class Readback>
void CheckFlood(DeviceType& device, const shader::CompiledShaderView& shader,
                Readback readbackBuffer) {
  using namespace flood_slice;
  ReflectedComputePipeline compute;
  CreateReflectedComputePipeline(device, shader, "flood", compute);
  if (testing::Test::HasFatalFailure()) {
    return;
  }
  auto output = device.createTexture({"flood output",
                                      {kWidth, kHeight},
                                      TextureFormat::RGBA32Float,
                                      TextureUsage::StorageBinding | TextureUsage::CopySrc});
  ASSERT_THAT(output, HasResult());
  auto outputView = device.createTextureView(output.result(), {"flood output"});
  ASSERT_THAT(outputView, HasResult());
  shader::programs::FloodParams params{};
  std::memcpy(params.color, kColor.data(), sizeof(params.color));
  auto uniform = device.createBuffer(
      {"flood parameters", sizeof(params), BufferUsage::Uniform | BufferUsage::CopyDst});
  ASSERT_THAT(uniform, HasResult());
  ASSERT_THAT(device.writeBuffer(uniform.result(), 0,
                                 {reinterpret_cast<const uint8_t*>(&params), sizeof(params)}),
              IsOk());
  auto group = device.createBindGroup(
      {"flood",
       compute.layout,
       {{ReflectedBinding(shader, "outputTexture"), TextureViewBinding{outputView.result()}},
        {ReflectedBinding(shader, "params"), BufferBinding{uniform.result(), 0, sizeof(params)}}}});
  ASSERT_THAT(group, HasResult());
  auto readback = device.createBuffer(
      {"flood readback", kRowBytes * kHeight, BufferUsage::CopyDst | BufferUsage::MapRead});
  ASSERT_THAT(readback, HasResult());
  auto encoder = device.createCommandEncoder();
  ASSERT_THAT(encoder, HasResult());
  auto pass = encoder.result()->beginComputePass({"flood"});
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
      std::array<float, 4> actual{};
      std::memcpy(actual.data(), bytes.result().data() + y * kRowBytes + x * sizeof(actual),
                  sizeof(actual));
      EXPECT_THAT(actual, testing::ElementsAreArray(kColor)) << "pixel=" << x << "," << y;
    }
  }
}

}  // namespace donner::gpu::tests
