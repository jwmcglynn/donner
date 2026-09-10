#pragma once
/// @file
/// Native morphology neighborhoods and float storage acceptance.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/tests/GpuTestUtils.h"

namespace donner::gpu::tests {

/// Runs erosion and dilation with transparent source borders and checks float values exactly.
/// @param device Native device with bounded wait/readback support.
/// @param shaderDescriptor Backend-emitted module with the shared cs_main entry point.
/// @param readbackBuffer Reads the submitted buffer through the backend's host mapping API.
template <typename DeviceType, typename Readback>
void CheckMorphologyStorage(DeviceType& device, const ShaderModuleDescriptor& shaderDescriptor,
                            Readback readbackBuffer, bool erode) {
  auto shader = device.createShaderModule(shaderDescriptor);
  ASSERT_THAT(shader, HasResult());
  auto layout = device.createBindGroupLayout(BindGroupLayoutDescriptor{
      "float",
      {{0, ShaderStage::Compute, BindingType::SampledTexture2dUnfilterableFloat},
       {1, ShaderStage::Compute, BindingType::WriteOnlyStorageTexture2d,
        TextureFormat::RGBA32Float},
       {2, ShaderStage::Compute, BindingType::UniformBuffer}}});
  ASSERT_THAT(layout, HasResult());
  auto pipelineLayout =
      device.createPipelineLayout(PipelineLayoutDescriptor{"float", {layout.result()}});
  ASSERT_THAT(pipelineLayout, HasResult());
  auto pipeline = device.createComputePipeline(ComputePipelineDescriptor{
      "float", pipelineLayout.result(), ComputeState{shader.result(), "cs_main"}, {8, 8, 1}});
  ASSERT_THAT(pipeline, HasResult());
  auto input =
      device.createTexture(TextureDescriptor{"morphology input",
                                             {4, 4},
                                             TextureFormat::RGBA32Float,
                                             TextureUsage::Sampled | TextureUsage::CopyDst});
  ASSERT_THAT(input, HasResult());
  auto output =
      device.createTexture(TextureDescriptor{"morphology output",
                                             {4, 4},
                                             TextureFormat::RGBA32Float,
                                             TextureUsage::StorageBinding | TextureUsage::CopySrc});
  ASSERT_THAT(output, HasResult());
  auto inputView = device.createTextureView(input.result(), TextureViewDescriptor{"input"});
  auto outputView = device.createTextureView(output.result(), TextureViewDescriptor{"output"});
  ASSERT_THAT(inputView, HasResult());
  ASSERT_THAT(outputView, HasResult());
  std::array<float, 64> values{};
  std::array<uint8_t, 1024> upload{};
  for (size_t i = 0; i < values.size(); i += 4) {
    values[i] = float(i) / 1024 + 0.000123f;
    values[i + 1] = 0.25f;
    values[i + 2] = 0.5f;
    values[i + 3] = 0.7f;
  }
  for (size_t y = 0; y < 4; ++y) {
    std::memcpy(upload.data() + y * 256, values.data() + y * 16, 16 * sizeof(float));
  }
  ASSERT_THAT(device.writeTexture(input.result(), upload, {0, 256, 4}, {4, 4}), IsOk());
  auto uniform = device.createBuffer(
      BufferDescriptor{"morphology parameters", 16, BufferUsage::Uniform | BufferUsage::CopyDst});
  ASSERT_THAT(uniform, HasResult());
  const std::array<int32_t, 4> params{1, 1, erode ? 0 : 1, 0};
  ASSERT_THAT(
      device.writeBuffer(uniform.result(), 0,
                         std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(params.data()),
                                                  sizeof(params))),
      IsOk());
  auto group =
      device.createBindGroup(BindGroupDescriptor{"float",
                                                 layout.result(),
                                                 {{0, TextureViewBinding{inputView.result()}},
                                                  {1, TextureViewBinding{outputView.result()}},
                                                  {2, BufferBinding{uniform.result(), 0, 16}}}});
  ASSERT_THAT(group, HasResult());
  auto readback = device.createBuffer(
      BufferDescriptor{"morphology readback", 1024, BufferUsage::CopyDst | BufferUsage::MapRead});
  ASSERT_THAT(readback, HasResult());
  auto encoder = device.createCommandEncoder();
  ASSERT_THAT(encoder, HasResult());
  auto pass = encoder.result()->beginComputePass(ComputePassDescriptor{"float"});
  ASSERT_THAT(pass, HasResult());
  ASSERT_THAT(pass.result()->setPipeline(pipeline.result()), IsOk());
  ASSERT_THAT(pass.result()->setBindGroup(0, group.result()), IsOk());
  ASSERT_THAT(pass.result()->dispatchWorkgroups(1, 1, 1), IsOk());
  ASSERT_THAT(pass.result()->end(), IsOk());
  ASSERT_THAT(encoder.result()->copyTextureToBuffer(TexelCopyTextureInfo{output.result()},
                                                    readback.result(), {0, 256, 4}, {4, 4}),
              IsOk());
  auto commands = encoder.result()->finish();
  ASSERT_THAT(commands, HasResult());
  auto serial = device.submit(std::move(commands).result());
  ASSERT_THAT(serial, HasResult());
  ASSERT_THAT(device.waitForSerial(serial.result(), 5.0), testing::IsTrue());
  const auto bytes = readbackBuffer(readback.result());
  ASSERT_THAT(bytes, HasResult());
  ASSERT_THAT(bytes.result(), testing::SizeIs(testing::Ge(1024u)));
  for (int32_t y = 0; y < 4; ++y) {
    for (int32_t x = 0; x < 4; ++x) {
      std::array<float, 4> expected{};
      expected.fill(erode ? 1.0f : 0.0f);
      for (int32_t dy = -1; dy <= 1; ++dy) {
        for (int32_t dx = -1; dx <= 1; ++dx) {
          const int32_t sx = x + dx, sy = y + dy;
          for (size_t c = 0; c < 4; ++c) {
            const float value =
                sx >= 0 && sy >= 0 && sx < 4 && sy < 4 ? values[(sy * 4 + sx) * 4 + c] : 0;
            expected[c] = erode ? std::min(expected[c], value) : std::max(expected[c], value);
          }
        }
      }
      std::array<float, 4> actual{};
      std::memcpy(actual.data(), bytes.result().data() + y * 256 + x * sizeof(actual),
                  sizeof(actual));
      EXPECT_THAT(actual, testing::ElementsAreArray(expected)) << "pixel=" << x << "," << y;
    }
  }
}

}  // namespace donner::gpu::tests
