#pragma once
/// @file
/// Native shadow composition, shared input bindings, and offset rounding acceptance.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
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

/// Composes a translucent blue shadow beneath a translucent red source at half-pixel offsets.
/// @param device Native device with bounded wait/readback support.
/// @param shaderDescriptor Backend-emitted module with the shared cs_main entry point.
/// @param readbackBuffer Reads the submitted buffer through the backend's host mapping API.
template <typename DeviceType, typename Readback>
void CheckDropShadowStorage(DeviceType& device, const ShaderModuleDescriptor& shaderDescriptor,
                            Readback readbackBuffer) {
  auto shader = device.createShaderModule(shaderDescriptor);
  ASSERT_THAT(shader, HasResult());
  auto layout = device.createBindGroupLayout(BindGroupLayoutDescriptor{
      "float",
      {{0, ShaderStage::Compute, BindingType::SampledTexture2dUnfilterableFloat},
       {1, ShaderStage::Compute, BindingType::SampledTexture2dUnfilterableFloat},
       {2, ShaderStage::Compute, BindingType::WriteOnlyStorageTexture2d,
        TextureFormat::RGBA32Float},
       {3, ShaderStage::Compute, BindingType::UniformBuffer}}});
  ASSERT_THAT(layout, HasResult());
  auto pipelineLayout =
      device.createPipelineLayout(PipelineLayoutDescriptor{"float", {layout.result()}});
  ASSERT_THAT(pipelineLayout, HasResult());
  auto pipeline = device.createComputePipeline(ComputePipelineDescriptor{
      "float", pipelineLayout.result(), ComputeState{shader.result(), "cs_main"}, {8, 8, 1}});
  ASSERT_THAT(pipeline, HasResult());
  const auto runCase = [&](bool aliasedInputs) {
    auto source =
        device.createTexture(TextureDescriptor{"shadow source",
                                               {4, 4},
                                               TextureFormat::RGBA32Float,
                                               TextureUsage::Sampled | TextureUsage::CopyDst});
    auto blurred =
        device.createTexture(TextureDescriptor{"shadow blur",
                                               {4, 4},
                                               TextureFormat::RGBA32Float,
                                               TextureUsage::Sampled | TextureUsage::CopyDst});
    auto output = device.createTexture(
        TextureDescriptor{"shadow output",
                          {4, 4},
                          TextureFormat::RGBA32Float,
                          TextureUsage::StorageBinding | TextureUsage::CopySrc});
    ASSERT_THAT(source, HasResult());
    ASSERT_THAT(blurred, HasResult());
    ASSERT_THAT(output, HasResult());
    auto sourceView = device.createTextureView(source.result(), TextureViewDescriptor{"source"});
    auto blurredView = device.createTextureView(blurred.result(), TextureViewDescriptor{"blurred"});
    auto outputView = device.createTextureView(output.result(), TextureViewDescriptor{"output"});
    ASSERT_THAT(sourceView, HasResult());
    ASSERT_THAT(blurredView, HasResult());
    ASSERT_THAT(outputView, HasResult());

    std::array<float, 64> sourceValues{};
    std::array<float, 64> blurredValues{};
    for (size_t y = 0; y < 4; ++y) {
      for (size_t x = 0; x < 4; ++x) {
        const size_t offset = (y * 4 + x) * 4;
        if (aliasedInputs) {
          sourceValues[offset] = 0.5f;
          sourceValues[offset + 3] = 0.5f;
        } else {
          const float sourceAlpha = float((x + 2 * y) % 4 + 1) / 4;
          sourceValues[offset] = float(x + 1) / 8 * sourceAlpha;
          sourceValues[offset + 1] = float(y + 1) / 8 * sourceAlpha;
          sourceValues[offset + 2] = 0.25f * sourceAlpha;
          sourceValues[offset + 3] = sourceAlpha;
          const float blurredAlpha = float((3 * x + y) % 5 + 1) / 6;
          blurredValues[offset] = float(4 - x) / 5 * blurredAlpha;
          blurredValues[offset + 1] = float(4 - y) / 5 * blurredAlpha;
          blurredValues[offset + 2] = 0.125f * blurredAlpha;
          blurredValues[offset + 3] = blurredAlpha;
        }
      }
    }
    std::array<uint8_t, 1024> sourceUpload{};
    std::array<uint8_t, 1024> blurredUpload{};
    for (size_t y = 0; y < 4; ++y) {
      std::memcpy(sourceUpload.data() + y * 256, sourceValues.data() + y * 16, 16 * sizeof(float));
      std::memcpy(blurredUpload.data() + y * 256, blurredValues.data() + y * 16,
                  16 * sizeof(float));
    }
    ASSERT_THAT(device.writeTexture(source.result(), sourceUpload, {0, 256, 4}, {4, 4}), IsOk());
    ASSERT_THAT(device.writeTexture(blurred.result(), blurredUpload, {0, 256, 4}, {4, 4}), IsOk());

    auto uniform = device.createBuffer(
        BufferDescriptor{"shadow parameters", 32, BufferUsage::Uniform | BufferUsage::CopyDst});
    ASSERT_THAT(uniform, HasResult());
    const std::array<float, 8> params{0, 0, 1, 0.5f, 0.5f, -0.5f, 0, 0};
    ASSERT_THAT(
        device.writeBuffer(uniform.result(), 0,
                           std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(params.data()),
                                                    sizeof(params))),
        IsOk());
    auto group = device.createBindGroup(BindGroupDescriptor{
        "float",
        layout.result(),
        {{0, TextureViewBinding{sourceView.result()}},
         {1, TextureViewBinding{aliasedInputs ? sourceView.result() : blurredView.result()}},
         {2, TextureViewBinding{outputView.result()}},
         {3, BufferBinding{uniform.result(), 0, 32}}}});
    ASSERT_THAT(group, HasResult());
    auto readback = device.createBuffer(
        BufferDescriptor{"shadow readback", 1024, BufferUsage::CopyDst | BufferUsage::MapRead});
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
    for (int32_t y = 0; y < 4; ++y) {
      std::memcpy(actual->data().data() + y * 16, bytes.result().data() + y * 256,
                  16 * sizeof(float));
      for (int32_t x = 0; x < 4; ++x) {
        const size_t offset = (y * 4 + x) * 4;
        std::copy_n(sourceValues.begin() + offset, 4, expected->data().begin() + offset);
        const int32_t sampleX = x - 1;
        const int32_t sampleY = y + 1;
        if (sampleX < 0 || sampleY >= 4) {
          continue;
        }
        const auto& alphaValues = aliasedInputs ? sourceValues : blurredValues;
        const float shadowAlpha = 0.5f * alphaValues[(sampleY * 4 + sampleX) * 4 + 3];
        const float remaining = 1 - sourceValues[offset + 3];
        expected->data()[offset + 2] += shadowAlpha * remaining;
        expected->data()[offset + 3] += shadowAlpha * remaining;
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
        aliasedInputs ? "drop_shadow_aliased_inputs" : "drop_shadow_distinct_inputs",
        editor::tests::PixelmatchIdentityParams());
  };
  runCase(true);
  runCase(false);
}

}  // namespace donner::gpu::tests
