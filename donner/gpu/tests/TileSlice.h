#pragma once
/// @file
/// Native tile wrapping and float storage acceptance.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/gpu/tests/ReflectedComputeSlice.h"

namespace donner::gpu::tests {

/// Runs signed-origin tiling with differing source/output extents and checks float values exactly.
/// @param device Native device with bounded wait/readback support.
/// @param shader Selected or mutation artifact; bindings and workgroup come from reflection.
/// @param readbackBuffer Reads the submitted buffer through the backend's host mapping API.
template <typename DeviceType, typename Readback>
void CheckTileStorage(DeviceType& device, const shader::CompiledShaderView& shader,
                      Readback readbackBuffer) {
  ReflectedComputePipeline compute;
  CreateReflectedComputePipeline(device, shader, "tile", compute);
  if (testing::Test::HasFatalFailure()) return;
  auto input =
      device.createTexture(TextureDescriptor{"tile input",
                                             {4, 2},
                                             TextureFormat::RGBA32Float,
                                             TextureUsage::Sampled | TextureUsage::CopyDst});
  ASSERT_THAT(input, HasResult());
  auto output =
      device.createTexture(TextureDescriptor{"tile output",
                                             {13, 3},
                                             TextureFormat::RGBA32Float,
                                             TextureUsage::StorageBinding | TextureUsage::CopySrc});
  ASSERT_THAT(output, HasResult());
  auto inputView = device.createTextureView(input.result(), TextureViewDescriptor{"input"});
  auto outputView = device.createTextureView(output.result(), TextureViewDescriptor{"output"});
  ASSERT_THAT(inputView, HasResult());
  ASSERT_THAT(outputView, HasResult());
  std::array<float, 32> values{};
  std::array<uint8_t, 512> upload{};
  for (size_t i = 0; i < values.size(); i += 4) {
    values[i] = float(i) / 1024 + 0.000123f;
    values[i + 1] = -0.5f;
    values[i + 2] = 1.5f;
    values[i + 3] = 0.7f;
  }
  for (size_t y = 0; y < 2; ++y) {
    std::memcpy(upload.data() + y * 256, values.data() + y * 16, 16 * sizeof(float));
  }
  ASSERT_THAT(device.writeTexture(input.result(), upload, {0, 256, 2}, {4, 2}), IsOk());
  auto uniform = device.createBuffer(
      BufferDescriptor{"tile parameters", 16, BufferUsage::Uniform | BufferUsage::CopyDst});
  ASSERT_THAT(uniform, HasResult());
  const std::array<int32_t, 4> params{1, -1, 3, 3};
  ASSERT_THAT(
      device.writeBuffer(uniform.result(), 0,
                         std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(params.data()),
                                                  sizeof(params))),
      IsOk());
  auto group = device.createBindGroup(BindGroupDescriptor{
      "tile",
      compute.layout,
      {{ReflectedBinding(shader, "inputTexture"), TextureViewBinding{inputView.result()}},
       {ReflectedBinding(shader, "outputTexture"), TextureViewBinding{outputView.result()}},
       {ReflectedBinding(shader, "params"), BufferBinding{uniform.result(), 0, 16}}}});
  ASSERT_THAT(group, HasResult());
  auto readback = device.createBuffer(
      BufferDescriptor{"tile readback", 768, BufferUsage::CopyDst | BufferUsage::MapRead});
  ASSERT_THAT(readback, HasResult());
  auto encoder = device.createCommandEncoder();
  ASSERT_THAT(encoder, HasResult());
  auto pass = encoder.result()->beginComputePass(ComputePassDescriptor{"float"});
  ASSERT_THAT(pass, HasResult());
  ASSERT_THAT(pass.result()->setPipeline(compute.pipeline), IsOk());
  ASSERT_THAT(pass.result()->setBindGroup(0, group.result()), IsOk());
  const auto groups = compute.groupsFor(13, 3);
  ASSERT_THAT(pass.result()->dispatchWorkgroups(groups[0], groups[1], groups[2]), IsOk());
  ASSERT_THAT(pass.result()->end(), IsOk());
  ASSERT_THAT(encoder.result()->copyTextureToBuffer(TexelCopyTextureInfo{output.result()},
                                                    readback.result(), {0, 256, 3}, {13, 3}),
              IsOk());
  auto commands = encoder.result()->finish();
  ASSERT_THAT(commands, HasResult());
  auto serial = device.submit(std::move(commands).result());
  ASSERT_THAT(serial, HasResult());
  ASSERT_THAT(device.waitForSerial(serial.result(), 5.0), testing::IsTrue());
  const auto bytes = readbackBuffer(readback.result());
  ASSERT_THAT(bytes, HasResult());
  ASSERT_THAT(bytes.result(), testing::SizeIs(testing::Ge(768u)));
  for (int32_t y = 0; y < 3; ++y) {
    for (int32_t x = 0; x < 13; ++x) {
      const int32_t sx = ((x - 1) % 3 + 3) % 3 + 1;
      const int32_t sy = std::clamp((y + 1) % 3 - 1, 0, 1);
      const size_t origin = size_t(sy * 4 + sx) * 4;
      std::array<float, 4> actual{};
      std::memcpy(actual.data(), bytes.result().data() + y * 256 + x * sizeof(actual),
                  sizeof(actual));
      EXPECT_THAT(actual, testing::ElementsAre(values[origin], values[origin + 1],
                                               values[origin + 2], values[origin + 3]))
          << "pixel=" << x << "," << y;
    }
  }
}

}  // namespace donner::gpu::tests
