#pragma once
/// @file
/// Native blend modes compared with the CPU oracle after quantizing both outputs to RGBA8.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <utility>

#include "donner/base/RcString.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/shader/programs/BlendBindings.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "tiny_skia/filter/Blend.h"

namespace donner::gpu::tests {

/// Requires exact quantized RGBA8 agreement across alpha boundaries and channel orderings.
/// @param device Native device with bounded wait/readback support.
/// @param shaderDescriptor Backend-emitted module with the shared cs_main entry point.
/// @param readbackBuffer Reads the submitted buffer through the backend's host mapping API.
/// @param mode Standard blend mode index (0..15).
template <typename DeviceType, typename Readback>
void CheckBlendStorage(DeviceType& device, const ShaderModuleDescriptor& shaderDescriptor,
                       Readback readbackBuffer, uint32_t mode) {
  auto shader = device.createShaderModule(shaderDescriptor);
  ASSERT_THAT(shader, HasResult());
  auto layout = device.createBindGroupLayout(BindGroupLayoutDescriptor{
      "blend",
      {{0, ShaderStage::Compute, BindingType::SampledTexture2dUnfilterableFloat},
       {1, ShaderStage::Compute, BindingType::SampledTexture2dUnfilterableFloat},
       {2, ShaderStage::Compute, BindingType::WriteOnlyStorageTexture2d,
        TextureFormat::RGBA32Float},
       {3, ShaderStage::Compute, BindingType::UniformBuffer}}});
  ASSERT_THAT(layout, HasResult());
  auto pipelineLayout =
      device.createPipelineLayout(PipelineLayoutDescriptor{"blend", {layout.result()}});
  ASSERT_THAT(pipelineLayout, HasResult());
  auto pipeline = device.createComputePipeline(ComputePipelineDescriptor{
      "blend",
      pipelineLayout.result(),
      ComputeState{shader.result(), RcString(shader::programs::kBlendEntryPoint)},
      {shader::programs::kBlendWorkgroupSize, shader::programs::kBlendWorkgroupSize, 1}});
  ASSERT_THAT(pipeline, HasResult());
  auto source =
      device.createTexture(TextureDescriptor{"blend source",
                                             {4, 4},
                                             TextureFormat::RGBA32Float,
                                             TextureUsage::Sampled | TextureUsage::CopyDst});
  ASSERT_THAT(source, HasResult());
  auto output =
      device.createTexture(TextureDescriptor{"blend output",
                                             {4, 4},
                                             TextureFormat::RGBA32Float,
                                             TextureUsage::StorageBinding | TextureUsage::CopySrc});
  ASSERT_THAT(output, HasResult());
  auto sourceView = device.createTextureView(source.result(), TextureViewDescriptor{"source"});
  auto outputView = device.createTextureView(output.result(), TextureViewDescriptor{"output"});
  ASSERT_THAT(sourceView, HasResult());
  ASSERT_THAT(outputView, HasResult());
  auto sourcePixels = tiny_skia::filter::FloatPixmap::fromSize(4, 4);
  auto backdropPixels = tiny_skia::filter::FloatPixmap::fromSize(4, 4);
  auto expectedPixels = tiny_skia::filter::FloatPixmap::fromSize(4, 4);
  ASSERT_THAT(sourcePixels.has_value(), testing::IsTrue());
  ASSERT_THAT(backdropPixels.has_value(), testing::IsTrue());
  ASSERT_THAT(expectedPixels.has_value(), testing::IsTrue());
  constexpr std::array<std::array<float, 3>, 16> colors{{{0.125f, 0.5f, 0.875f},
                                                         {0.875f, 0.125f, 0.5f},
                                                         {0.5f, 0.875f, 0.125f},
                                                         {0.5f, 0.125f, 0.875f},
                                                         {0.875f, 0.5f, 0.125f},
                                                         {0.125f, 0.875f, 0.5f},
                                                         {0.25f, 0.25f, 0.75f},
                                                         {0.25f, 0.75f, 0.25f},
                                                         {0.75f, 0.25f, 0.25f},
                                                         {0.75f, 0.75f, 0.25f},
                                                         {0.75f, 0.25f, 0.75f},
                                                         {0.25f, 0.75f, 0.75f},
                                                         {0, 0, 0},
                                                         {0.5f, 0.5f, 0.5f},
                                                         {1, 1, 1},
                                                         {0, 0.5f, 1}}};
  constexpr std::array<float, 4> alphas{0, 0.25f, 0.5f, 1};
  for (size_t pixel = 0; pixel < 16; ++pixel) {
    const float sourceAlpha = alphas[pixel % 4], backdropAlpha = alphas[pixel / 4];
    for (size_t channel = 0; channel < 3; ++channel) {
      sourcePixels->data()[pixel * 4 + channel] =
          colors[pixel % colors.size()][channel] * sourceAlpha;
      backdropPixels->data()[pixel * 4 + channel] =
          colors[(pixel + 3) % colors.size()][channel] * backdropAlpha;
    }
    sourcePixels->data()[pixel * 4 + 3] = sourceAlpha;
    backdropPixels->data()[pixel * 4 + 3] = backdropAlpha;
  }
  tiny_skia::filter::blend(*backdropPixels, *sourcePixels, *expectedPixels,
                           static_cast<tiny_skia::filter::BlendMode>(mode));
  auto backdrop =
      device.createTexture(TextureDescriptor{"backdrop",
                                             {4, 4},
                                             TextureFormat::RGBA32Float,
                                             TextureUsage::Sampled | TextureUsage::CopyDst});
  ASSERT_THAT(backdrop, HasResult());
  auto backdropView =
      device.createTextureView(backdrop.result(), TextureViewDescriptor{"backdrop"});
  ASSERT_THAT(backdropView, HasResult());
  const auto uploadPixels = [&](const Texture& texture,
                                const tiny_skia::filter::FloatPixmap& pixels) {
    std::array<uint8_t, 1024> upload{};
    for (size_t y = 0; y < 4; ++y) {
      std::memcpy(upload.data() + y * 256, pixels.data().data() + y * 16, 16 * sizeof(float));
    }
    EXPECT_THAT(device.writeTexture(texture, upload, {0, 256, 4}, {4, 4}), IsOk());
  };
  uploadPixels(source.result(), *sourcePixels);
  uploadPixels(backdrop.result(), *backdropPixels);
  auto uniform = device.createBuffer(
      BufferDescriptor{"blend parameters", 16, BufferUsage::Uniform | BufferUsage::CopyDst});
  ASSERT_THAT(uniform, HasResult());
  const std::array<uint32_t, 4> params{mode, 0, 0, 0};
  ASSERT_THAT(
      device.writeBuffer(uniform.result(), 0,
                         std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(params.data()),
                                                  sizeof(params))),
      IsOk());
  auto group =
      device.createBindGroup(BindGroupDescriptor{"blend",
                                                 layout.result(),
                                                 {{0, TextureViewBinding{sourceView.result()}},
                                                  {1, TextureViewBinding{backdropView.result()}},
                                                  {2, TextureViewBinding{outputView.result()}},
                                                  {3, BufferBinding{uniform.result(), 0, 16}}}});
  ASSERT_THAT(group, HasResult());
  auto readback = device.createBuffer(
      BufferDescriptor{"blend readback", 1024, BufferUsage::CopyDst | BufferUsage::MapRead});
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
  svg::RendererBitmap actualBitmap;
  actualBitmap.dimensions = {4, 4};
  actualBitmap.rowBytes = 16;
  actualBitmap.pixels.resize(64);
  svg::RendererBitmap expectedBitmap;
  expectedBitmap.dimensions = actualBitmap.dimensions;
  expectedBitmap.rowBytes = actualBitmap.rowBytes;
  expectedBitmap.pixels.resize(64);
  for (int32_t y = 0; y < 4; ++y) {
    for (int32_t x = 0; x < 4; ++x) {
      std::array<float, 4> actual{};
      std::memcpy(actual.data(), bytes.result().data() + y * 256 + x * sizeof(actual),
                  sizeof(actual));
      std::array<uint8_t, 4> actualBytes{}, expectedBytes{};
      for (size_t c = 0; c < 4; ++c) {
        ASSERT_THAT(std::isfinite(actual[c]), testing::IsTrue());
        actualBytes[c] = uint8_t(std::floor(std::clamp(actual[c], 0.0f, 1.0f) * 255.0f + 0.5f));
        expectedBytes[c] = uint8_t(std::floor(
            std::clamp(expectedPixels->data()[(y * 4 + x) * 4 + c], 0.0f, 1.0f) * 255.0f + 0.5f));
      }
      const size_t offset = y * actualBitmap.rowBytes + x * 4;
      std::copy(actualBytes.begin(), actualBytes.end(), actualBitmap.pixels.begin() + offset);
      std::copy(expectedBytes.begin(), expectedBytes.end(), expectedBitmap.pixels.begin() + offset);
    }
  }
  editor::tests::CompareBitmapToBitmap(actualBitmap, expectedBitmap,
                                       "native_blend_mode_" + std::to_string(mode),
                                       editor::tests::PixelmatchIdentityParams());
}

}  // namespace donner::gpu::tests
