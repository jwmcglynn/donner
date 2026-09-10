#pragma once
/// @file
/// Native component-transfer packed-table and gamma-guard acceptance.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/tests/GpuTestUtils.h"

namespace donner::gpu::tests {

/// Checks maximum packed tables and gamma guards with exact dyadic float results.
/// @param device Native device with bounded wait/readback support.
/// @param shaderDescriptor Backend-emitted module with the shared cs_main entry point.
/// @param readbackBuffer Reads the submitted buffer through the backend's host mapping API.
template <typename DeviceType, typename Readback>
void CheckComponentTransferStorage(DeviceType& device,
                                   const ShaderModuleDescriptor& shaderDescriptor,
                                   Readback readbackBuffer, uint32_t mode) {
  auto shader = device.createShaderModule(shaderDescriptor);
  ASSERT_THAT(shader, HasResult());
  auto layout = device.createBindGroupLayout(BindGroupLayoutDescriptor{
      "float",
      {{0, ShaderStage::Compute, BindingType::SampledTexture2dUnfilterableFloat},
       {1, ShaderStage::Compute, BindingType::WriteOnlyStorageTexture2d,
        TextureFormat::RGBA32Float},
       {2, ShaderStage::Compute, BindingType::ReadOnlyStorageBuffer}}});
  ASSERT_THAT(layout, HasResult());
  auto pipelineLayout =
      device.createPipelineLayout(PipelineLayoutDescriptor{"float", {layout.result()}});
  ASSERT_THAT(pipelineLayout, HasResult());
  auto pipeline = device.createComputePipeline(ComputePipelineDescriptor{
      "float", pipelineLayout.result(), ComputeState{shader.result(), "cs_main"}, {8, 8, 1}});
  ASSERT_THAT(pipeline, HasResult());
  auto input =
      device.createTexture(TextureDescriptor{"component transfer input",
                                             {4, 4},
                                             TextureFormat::RGBA32Float,
                                             TextureUsage::Sampled | TextureUsage::CopyDst});
  ASSERT_THAT(input, HasResult());
  auto output =
      device.createTexture(TextureDescriptor{"component transfer output",
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
    const float alpha = (i % 16) == 0 ? 0.0f : 0.5f;
    values[i] = 0.25f * alpha;
    values[i + 1] = 0.5f * alpha;
    values[i + 2] = 0.75f * alpha;
    values[i + 3] = alpha;
  }
  for (size_t y = 0; y < 4; ++y) {
    std::memcpy(upload.data() + y * 256, values.data() + y * 16, 16 * sizeof(float));
  }
  ASSERT_THAT(device.writeTexture(input.result(), upload, {0, 256, 4}, {4, 4}), IsOk());
  std::array<float, 32 + 4096> params{};
  for (size_t channel = 0; channel < 4; ++channel) {
    const size_t base = channel * 8;
    params[base] =
        mode == 0 ? float(1 + channel % 2) : (mode == 1 ? 4.0f : (channel == 0 ? 255.0f : 0.0f));
    params[base + 1] = float(channel * 1024);
    params[base + 2] = mode == 0 ? 1024.0f : 0.0f;
    params[base + 5] = channel == 0 ? 0.0f : 0.25f;
    params[base + 6] = channel == 0 ? -2.0f : 0.0f;
    params[base + 7] = 0.25f;
    std::fill_n(params.begin() + 32 + channel * 1024, 1024, float(channel + 1) * 0.25f);
  }
  auto uniform =
      device.createBuffer(BufferDescriptor{"component transfer parameters", sizeof(params),
                                           BufferUsage::Storage | BufferUsage::CopyDst});
  ASSERT_THAT(uniform, HasResult());
  ASSERT_THAT(
      device.writeBuffer(uniform.result(), 0,
                         std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(params.data()),
                                                  sizeof(params))),
      IsOk());
  auto group = device.createBindGroup(
      BindGroupDescriptor{"float",
                          layout.result(),
                          {{0, TextureViewBinding{inputView.result()}},
                           {1, TextureViewBinding{outputView.result()}},
                           {2, BufferBinding{uniform.result(), 0, sizeof(params)}}}});
  ASSERT_THAT(group, HasResult());
  auto readback = device.createBuffer(BufferDescriptor{
      "component transfer readback", 1024, BufferUsage::CopyDst | BufferUsage::MapRead});
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
      const std::array<float, 4> expected =
          mode == 0 ? std::array<float, 4>{0.25f, 0.5f, 0.75f, 1.0f}
          : mode == 1
              ? std::array<float, 4>{0.125f, 0.25f, 0.25f, 0.5f}
              : std::array<float, 4>{values[(y * 4 + x) * 4], values[(y * 4 + x) * 4 + 1],
                                     values[(y * 4 + x) * 4 + 2], values[(y * 4 + x) * 4 + 3]};
      std::array<float, 4> actual{};
      std::memcpy(actual.data(), bytes.result().data() + y * 256 + x * sizeof(actual),
                  sizeof(actual));
      EXPECT_THAT(actual, testing::ElementsAreArray(expected)) << "pixel=" << x << "," << y;
    }
  }
}

}  // namespace donner::gpu::tests
