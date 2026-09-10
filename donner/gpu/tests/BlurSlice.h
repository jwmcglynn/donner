#pragma once
/// @file
/// Native Gaussian/box blur and folded clipping acceptance.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/tests/GpuTestUtils.h"

namespace donner::gpu::tests {

/// Runs Gaussian and asymmetric box passes with folded clipping on a constant float image.
/// @param device Native device with bounded wait/readback support.
/// @param shaderDescriptor Backend-emitted module with the shared cs_main entry point.
/// @param readbackBuffer Reads the submitted buffer through the backend's host mapping API.
template <typename DeviceType, typename Readback>
void CheckBlurStorage(DeviceType& device, const ShaderModuleDescriptor& shaderDescriptor,
                      Readback readbackBuffer, float sigma, uint32_t kernelType, uint32_t axis) {
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
      device.createTexture(TextureDescriptor{"blur input",
                                             {4, 4},
                                             TextureFormat::RGBA32Float,
                                             TextureUsage::Sampled | TextureUsage::CopyDst});
  ASSERT_THAT(input, HasResult());
  auto output =
      device.createTexture(TextureDescriptor{"blur output",
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
    values[i] = 0.125f;
    values[i + 1] = 0.25f;
    values[i + 2] = 0.5f;
    values[i + 3] = 1.0f;
  }
  for (size_t y = 0; y < 4; ++y) {
    std::memcpy(upload.data() + y * 256, values.data() + y * 16, 16 * sizeof(float));
  }
  ASSERT_THAT(device.writeTexture(input.result(), upload, {0, 256, 4}, {4, 4}), IsOk());
  auto uniform = device.createBuffer(
      BufferDescriptor{"blur parameters", 48, BufferUsage::Uniform | BufferUsage::CopyDst});
  ASSERT_THAT(uniform, HasResult());
  const std::array<uint32_t, 12> params{
      std::bit_cast<uint32_t>(sigma), axis, 1, kernelType, 1, 2, 1, 1, 3, 3, 1, 0};
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
                                                  {2, BufferBinding{uniform.result(), 0, 48}}}});
  ASSERT_THAT(group, HasResult());
  auto readback = device.createBuffer(
      BufferDescriptor{"blur readback", 1024, BufferUsage::CopyDst | BufferUsage::MapRead});
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
  std::vector<uint8_t> actualPixels(4 * 4 * 4);
  std::vector<uint8_t> expectedPixels(4 * 4 * 4);
  for (int32_t y = 0; y < 4; ++y) {
    for (int32_t x = 0; x < 4; ++x) {
      std::array<float, 4> actual{};
      std::memcpy(actual.data(), bytes.result().data() + y * 256 + x * sizeof(actual),
                  sizeof(actual));
      ASSERT_THAT(actual,
                  testing::Each(testing::Truly([](float value) { return std::isfinite(value); })))
          << "pixel=" << x << "," << y;
      const size_t pixelOffset = (y * 4 + x) * 4;
      std::transform(actual.begin(), actual.end(), actualPixels.begin() + pixelOffset,
                     [](float value) {
                       return static_cast<uint8_t>(std::round(std::clamp(value, 0.0f, 1.0f) * 255));
                     });
      if (x >= 1 && x < 3 && y >= 1 && y < 3) {
        const std::array<uint8_t, 4> expected{32, 64, 128, 255};
        std::copy(expected.begin(), expected.end(), expectedPixels.begin() + pixelOffset);
      }
    }
  }
  editor::tests::CompareBitmapToBitmap(
      svg::RendererBitmap{Vector2i(4, 4), std::move(actualPixels), 16},
      svg::RendererBitmap{Vector2i(4, 4), std::move(expectedPixels), 16},
      "blur_axis_" + std::to_string(axis) + "_kernel_" + std::to_string(kernelType) + "_sigma_" +
          std::to_string(sigma),
      editor::tests::PixelmatchIdentityParams());
}

}  // namespace donner::gpu::tests
