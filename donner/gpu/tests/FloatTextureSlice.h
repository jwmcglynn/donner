#pragma once
/// @file
/// Native float-texture upload, sampled compute dispatch, and readback acceptance.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <utility>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/tests/GpuTestUtils.h"

namespace donner::gpu::tests {

/// Runs the shared float-storage module and checks all four returned float values exactly.
/// @param device Native device with bounded wait/readback support.
/// @param shaderDescriptor Backend-emitted module with the shared cs_main entry point.
template <typename DeviceType>
void CheckFloatTextureStorage(DeviceType& device, const ShaderModuleDescriptor& shaderDescriptor) {
  auto shader = device.createShaderModule(shaderDescriptor);
  ASSERT_THAT(shader, HasResult());
  auto layout = device.createBindGroupLayout(BindGroupLayoutDescriptor{
      "float",
      {{0, ShaderStage::Compute, BindingType::SampledTexture2dUnfilterableFloat},
       {1, ShaderStage::Compute, BindingType::WriteOnlyStorageTexture2d,
        TextureFormat::RGBA32Float}}});
  ASSERT_THAT(layout, HasResult());
  auto pipelineLayout =
      device.createPipelineLayout(PipelineLayoutDescriptor{"float", {layout.result()}});
  ASSERT_THAT(pipelineLayout, HasResult());
  auto pipeline = device.createComputePipeline(ComputePipelineDescriptor{
      "float", pipelineLayout.result(), ComputeState{shader.result(), "cs_main"}, {1, 1, 1}});
  ASSERT_THAT(pipeline, HasResult());
  auto input =
      device.createTexture(TextureDescriptor{"float input",
                                             {1, 1},
                                             TextureFormat::RGBA32Float,
                                             TextureUsage::Sampled | TextureUsage::CopyDst});
  ASSERT_THAT(input, HasResult());
  auto output =
      device.createTexture(TextureDescriptor{"float output",
                                             {1, 1},
                                             TextureFormat::RGBA32Float,
                                             TextureUsage::StorageBinding | TextureUsage::CopySrc});
  ASSERT_THAT(output, HasResult());
  auto inputView = device.createTextureView(input.result(), TextureViewDescriptor{"input"});
  auto outputView = device.createTextureView(output.result(), TextureViewDescriptor{"output"});
  ASSERT_THAT(inputView, HasResult());
  ASSERT_THAT(outputView, HasResult());
  const std::array<float, 4> values{0.125f, 0.25f, 0.5f, 0.75f};
  std::array<uint8_t, 256> upload{};
  std::memcpy(upload.data(), values.data(), sizeof(values));
  ASSERT_THAT(device.writeTexture(input.result(), upload, {0, 256, 1}, {1, 1}), IsOk());
  auto group = device.createBindGroup(BindGroupDescriptor{
      "float",
      layout.result(),
      {{0, TextureViewBinding{inputView.result()}}, {1, TextureViewBinding{outputView.result()}}}});
  ASSERT_THAT(group, HasResult());
  auto readback = device.createBuffer(
      BufferDescriptor{"float readback", 256, BufferUsage::CopyDst | BufferUsage::MapRead});
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
                                                    readback.result(), {0, 256, 1}, {1, 1}),
              IsOk());
  auto commands = encoder.result()->finish();
  ASSERT_THAT(commands, HasResult());
  auto serial = device.submit(std::move(commands).result());
  ASSERT_THAT(serial, HasResult());
  ASSERT_THAT(device.waitForSerial(serial.result(), 5.0), testing::IsTrue());
  const auto bytes = device.readBackBuffer(readback.result());
  ASSERT_THAT(bytes, HasResult());
  ASSERT_THAT(bytes.result(), testing::SizeIs(testing::Ge(sizeof(values))));
  std::array<float, 4> actual{};
  std::memcpy(actual.data(), bytes.result().data(), sizeof(actual));
  constexpr float increment = 1.0f / 4096.0f;
  EXPECT_THAT(actual, testing::ElementsAre(0.125f + increment, 0.25f + increment, 0.5f + increment,
                                           0.75f + increment));
  EXPECT_THAT(device.lastErrorForTest(), testing::IsEmpty());
}

}  // namespace donner::gpu::tests
