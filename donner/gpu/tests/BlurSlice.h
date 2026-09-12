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
#include "tiny_skia/filter/GaussianBlur.h"

namespace donner::gpu::tests {

/// Runs Gaussian and asymmetric box passes with folded clipping on a varied float image.
/// @param device Native device with bounded wait/readback support.
/// @param shaderDescriptor Backend-emitted module with the shared cs_main entry point.
/// @param readbackBuffer Reads the submitted buffer through the backend's host mapping API.
/// @param sigma Gaussian standard deviation in pixels, zero for the copy path.
/// @param kernelType Zero for Gaussian, one for an asymmetric box.
/// @param axis Zero for horizontal, one for vertical.
/// @param edgeMode Zero for transparent, one for duplicate, two for wrap.
template <typename DeviceType, typename Readback>
void CheckBlurStorage(DeviceType& device, const ShaderModuleDescriptor& shaderDescriptor,
                      Readback readbackBuffer, float sigma, uint32_t kernelType, uint32_t axis,
                      uint32_t edgeMode) {
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
  for (size_t y = 0; y < 4; ++y) {
    for (size_t x = 0; x < 4; ++x) {
      const size_t offset = (y * 4 + x) * 4;
      const float alpha = float((x + 2 * y) % 4 + 1) / 4;
      values[offset] = float(x + 1) / 8 * alpha;
      values[offset + 1] = float(y + 1) / 8 * alpha;
      values[offset + 2] = 0.25f * alpha;
      values[offset + 3] = alpha;
    }
  }
  for (size_t y = 0; y < 4; ++y) {
    std::memcpy(upload.data() + y * 256, values.data() + y * 16, 16 * sizeof(float));
  }
  ASSERT_THAT(device.writeTexture(input.result(), upload, {0, 256, 4}, {4, 4}), IsOk());
  auto uniform = device.createBuffer(
      BufferDescriptor{"blur parameters", 48, BufferUsage::Uniform | BufferUsage::CopyDst});
  ASSERT_THAT(uniform, HasResult());
  const std::array<uint32_t, 12> params{
      std::bit_cast<uint32_t>(sigma), axis, edgeMode, kernelType, 1, 2, 0, 0, 3, 3, 1, 0};
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
  auto actual = tiny_skia::filter::FloatPixmap::fromSize(4, 4);
  auto expected = tiny_skia::filter::FloatPixmap::fromSize(4, 4);
  ASSERT_THAT(actual.has_value(), testing::IsTrue());
  ASSERT_THAT(expected.has_value(), testing::IsTrue());
  std::copy(values.begin(), values.end(), expected->data().begin());
  if (kernelType == 1) {
    for (int32_t y = 0; y < 4; ++y) {
      for (int32_t x = 0; x < 4; ++x) {
        for (size_t channel = 0; channel < 4; ++channel) {
          float sum = 0;
          for (int32_t tap = -1; tap <= 2; ++tap) {
            int32_t sx = x + (axis == 0 ? tap : 0);
            int32_t sy = y + (axis == 1 ? tap : 0);
            if (edgeMode == 0 && (sx < 0 || sy < 0 || sx >= 4 || sy >= 4)) {
              continue;
            }
            if (edgeMode == 2) {
              sx = (sx % 4 + 4) % 4;
              sy = (sy % 4 + 4) % 4;
            } else {
              sx = std::clamp(sx, 0, 3);
              sy = std::clamp(sy, 0, 3);
            }
            sum += values[(sy * 4 + sx) * 4 + channel];
          }
          expected->data()[(y * 4 + x) * 4 + channel] = sum / 4;
        }
      }
    }
  } else {
    tiny_skia::filter::gaussianBlur(*expected, axis == 0 ? sigma : 0, axis == 1 ? sigma : 0,
                                    static_cast<tiny_skia::filter::BlurEdgeMode>(edgeMode));
  }
  for (int32_t y = 0; y < 4; ++y) {
    std::memcpy(actual->data().data() + y * 16, bytes.result().data() + y * 256,
                16 * sizeof(float));
    for (int32_t x = 0; x < 4; ++x) {
      if (x >= 3 || y >= 3) {
        std::fill_n(expected->data().begin() + (y * 4 + x) * 4, 4, 0.0f);
      }
    }
  }
  ASSERT_THAT(actual->data(),
              testing::Each(testing::Truly([](float value) { return std::isfinite(value); })));
  const auto actualPixels = actual->toPixmap();
  const auto expectedPixels = expected->toPixmap();
  editor::tests::CompareBitmapToBitmap(
      svg::RendererBitmap{
          Vector2i(4, 4),
          std::vector<uint8_t>(actualPixels.data().begin(), actualPixels.data().end()), 16},
      svg::RendererBitmap{
          Vector2i(4, 4),
          std::vector<uint8_t>(expectedPixels.data().begin(), expectedPixels.data().end()), 16},
      "blur_axis_" + std::to_string(axis) + "_kernel_" + std::to_string(kernelType) + "_sigma_" +
          std::to_string(sigma) + "_edge_" + std::to_string(edgeMode),
      editor::tests::PixelmatchIdentityParams());
}

}  // namespace donner::gpu::tests
