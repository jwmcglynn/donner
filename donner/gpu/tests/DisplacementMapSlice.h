#pragma once
/// @file
/// Native displacement channel, bilinear interpolation, and transparent-border acceptance.

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
#include "donner/gpu/shader/programs/DisplacementMapBindings.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "tiny_skia/filter/GaussianBlur.h"

namespace donner::gpu::tests {

namespace displacement_map_details {

inline constexpr uint32_t kWidth = 5;
inline constexpr uint32_t kHeight = 4;
inline constexpr uint32_t kFloatsPerPixel = 4;
inline constexpr uint32_t kPackedRowBytes = kWidth * kFloatsPerPixel * sizeof(float);
inline constexpr uint32_t kTransferRowBytes = 256;

struct Scenario {
  float scale;
  uint32_t xChannel;
  uint32_t yChannel;
  const char* name;
};

inline constexpr std::array<Scenario, 5> kScenarios{{
    {0.0f, 0, 1, "zero_scale_rg"},
    {1.5f, 0, 1, "positive_scale_rg"},
    {-2.25f, 2, 3, "negative_scale_ba"},
    {2.0f, 3, 2, "positive_scale_ab"},
    {0.75f, 1, 0, "fractional_scale_gr"},
}};

inline float SelectStraightChannel(const std::array<float, 4>& sample, uint32_t channel) {
  if (channel == 3) {
    return sample[3];
  }
  return sample[3] > 0 ? std::min(1.0f, sample[channel] / sample[3]) : 0.0f;
}

inline std::array<float, 4> BilinearSample(const std::array<float, kWidth * kHeight * 4>& source,
                                           float x, float y) {
  const int32_t x0 = static_cast<int32_t>(std::floor(x));
  const int32_t y0 = static_cast<int32_t>(std::floor(y));
  const float tx = x - static_cast<float>(x0);
  const float ty = y - static_cast<float>(y0);
  const auto sample = [&](int32_t sampleX, int32_t sampleY, size_t channel) {
    if (sampleX < 0 || sampleY < 0 || sampleX >= static_cast<int32_t>(kWidth) ||
        sampleY >= static_cast<int32_t>(kHeight)) {
      return 0.0f;
    }
    return source[(sampleY * kWidth + sampleX) * 4 + channel];
  };
  std::array<float, 4> result{};
  for (size_t channel = 0; channel < result.size(); ++channel) {
    const float top =
        sample(x0, y0, channel) + tx * (sample(x0 + 1, y0, channel) - sample(x0, y0, channel));
    const float bottom = sample(x0, y0 + 1, channel) +
                         tx * (sample(x0 + 1, y0 + 1, channel) - sample(x0, y0 + 1, channel));
    result[channel] = std::clamp(top + ty * (bottom - top), 0.0f, 1.0f);
  }
  return result;
}

}  // namespace displacement_map_details

/**
 * Runs generated displacement-map code against an independent host oracle.
 *
 * The source and map are distinct and nonuniform. Map alpha varies down to zero, so RGB selectors
 * must unpremultiply while the alpha selector remains direct. The scenarios cover every selector,
 * zero and negative scale, fractional sampling, and transparent border taps.
 *
 * @param device Native device with bounded wait/readback support.
 * @param shaderDescriptor Build-generated platform descriptor.
 * @param readbackBuffer Reads a submitted buffer through the backend's host mapping API.
 */
template <typename DeviceType, typename Readback>
void CheckDisplacementMapStorage(DeviceType& device, const ShaderModuleDescriptor& shaderDescriptor,
                                 Readback readbackBuffer) {
  using namespace displacement_map_details;
  using shader::programs::DisplacementMapBinding;
  const auto binding = [](DisplacementMapBinding value) { return static_cast<uint32_t>(value); };

  auto shader = device.createShaderModule(shaderDescriptor);
  ASSERT_THAT(shader, HasResult());
  auto layout = device.createBindGroupLayout(BindGroupLayoutDescriptor{
      "displacement",
      {{binding(DisplacementMapBinding::SourceTexture), ShaderStage::Compute,
        BindingType::SampledTexture2dUnfilterableFloat},
       {binding(DisplacementMapBinding::MapTexture), ShaderStage::Compute,
        BindingType::SampledTexture2dUnfilterableFloat},
       {binding(DisplacementMapBinding::OutputTexture), ShaderStage::Compute,
        BindingType::WriteOnlyStorageTexture2d, TextureFormat::RGBA32Float},
       {binding(DisplacementMapBinding::Params), ShaderStage::Compute,
        BindingType::UniformBuffer}}});
  ASSERT_THAT(layout, HasResult());
  auto pipelineLayout =
      device.createPipelineLayout(PipelineLayoutDescriptor{"displacement", {layout.result()}});
  ASSERT_THAT(pipelineLayout, HasResult());
  auto pipeline = device.createComputePipeline(ComputePipelineDescriptor{
      "displacement",
      pipelineLayout.result(),
      ComputeState{shader.result(), RcString(shader::programs::kDisplacementMapEntryPoint)},
      {shader::programs::kDisplacementMapWorkgroupSize,
       shader::programs::kDisplacementMapWorkgroupSize, 1}});
  ASSERT_THAT(pipeline, HasResult());

  auto source =
      device.createTexture(TextureDescriptor{"displacement source",
                                             {kWidth, kHeight},
                                             TextureFormat::RGBA32Float,
                                             TextureUsage::Sampled | TextureUsage::CopyDst});
  auto map = device.createTexture(TextureDescriptor{"displacement map",
                                                    {kWidth, kHeight},
                                                    TextureFormat::RGBA32Float,
                                                    TextureUsage::Sampled | TextureUsage::CopyDst});
  ASSERT_THAT(source, HasResult());
  ASSERT_THAT(map, HasResult());
  auto sourceView = device.createTextureView(source.result(), TextureViewDescriptor{"source"});
  auto mapView = device.createTextureView(map.result(), TextureViewDescriptor{"map"});
  ASSERT_THAT(sourceView, HasResult());
  ASSERT_THAT(mapView, HasResult());

  std::array<float, kWidth * kHeight * 4> sourceValues{};
  std::array<float, kWidth * kHeight * 4> mapValues{};
  for (uint32_t y = 0; y < kHeight; ++y) {
    for (uint32_t x = 0; x < kWidth; ++x) {
      const size_t offset = (y * kWidth + x) * 4;
      const float sourceAlpha = static_cast<float>((x + 2 * y) % 4 + 1) / 4.0f;
      sourceValues[offset] = (static_cast<float>(x) + 1) / 6.0f * sourceAlpha;
      sourceValues[offset + 1] = (static_cast<float>(y) + 1) / 5.0f * sourceAlpha;
      sourceValues[offset + 2] = static_cast<float>((2 * x + y) % 5) / 5.0f * sourceAlpha;
      sourceValues[offset + 3] = sourceAlpha;

      const float mapAlpha = static_cast<float>((3 * x + y) % 4) / 3.0f;
      mapValues[offset] = (static_cast<float>(x) + 1) / 6.0f * mapAlpha;
      mapValues[offset + 1] = (static_cast<float>(y) + 1) / 5.0f * mapAlpha;
      mapValues[offset + 2] = static_cast<float>((x + 3 * y) % 5) / 4.0f * mapAlpha;
      mapValues[offset + 3] = mapAlpha;
    }
  }

  std::array<uint8_t, kTransferRowBytes * kHeight> sourceUpload{};
  std::array<uint8_t, kTransferRowBytes * kHeight> mapUpload{};
  for (uint32_t y = 0; y < kHeight; ++y) {
    std::memcpy(sourceUpload.data() + y * kTransferRowBytes, sourceValues.data() + y * kWidth * 4,
                kPackedRowBytes);
    std::memcpy(mapUpload.data() + y * kTransferRowBytes, mapValues.data() + y * kWidth * 4,
                kPackedRowBytes);
  }
  ASSERT_THAT(device.writeTexture(source.result(), sourceUpload, {0, kTransferRowBytes, kHeight},
                                  {kWidth, kHeight}),
              IsOk());
  ASSERT_THAT(device.writeTexture(map.result(), mapUpload, {0, kTransferRowBytes, kHeight},
                                  {kWidth, kHeight}),
              IsOk());

  for (const Scenario& scenario : kScenarios) {
    SCOPED_TRACE(scenario.name);
    auto output = device.createTexture(
        TextureDescriptor{scenario.name,
                          {kWidth, kHeight},
                          TextureFormat::RGBA32Float,
                          TextureUsage::StorageBinding | TextureUsage::CopySrc});
    ASSERT_THAT(output, HasResult());
    auto outputView = device.createTextureView(output.result(), TextureViewDescriptor{"output"});
    ASSERT_THAT(outputView, HasResult());

    struct Params {
      float scale;
      uint32_t xChannel;
      uint32_t yChannel;
      uint32_t padding;
    };
    const Params params{scenario.scale, scenario.xChannel, scenario.yChannel, 0};
    auto uniform = device.createBuffer(BufferDescriptor{
        "displacement parameters", sizeof(params), BufferUsage::Uniform | BufferUsage::CopyDst});
    ASSERT_THAT(uniform, HasResult());
    ASSERT_THAT(device.writeBuffer(uniform.result(), 0,
                                   std::span<const uint8_t>(
                                       reinterpret_cast<const uint8_t*>(&params), sizeof(params))),
                IsOk());
    auto group = device.createBindGroup(BindGroupDescriptor{
        scenario.name,
        layout.result(),
        {{binding(DisplacementMapBinding::SourceTexture), TextureViewBinding{sourceView.result()}},
         {binding(DisplacementMapBinding::MapTexture), TextureViewBinding{mapView.result()}},
         {binding(DisplacementMapBinding::OutputTexture), TextureViewBinding{outputView.result()}},
         {binding(DisplacementMapBinding::Params),
          BufferBinding{uniform.result(), 0, sizeof(params)}}}});
    ASSERT_THAT(group, HasResult());
    auto readback =
        device.createBuffer(BufferDescriptor{"displacement readback", kTransferRowBytes * kHeight,
                                             BufferUsage::CopyDst | BufferUsage::MapRead});
    ASSERT_THAT(readback, HasResult());
    auto encoder = device.createCommandEncoder();
    ASSERT_THAT(encoder, HasResult());
    auto pass = encoder.result()->beginComputePass(ComputePassDescriptor{scenario.name});
    ASSERT_THAT(pass, HasResult());
    ASSERT_THAT(pass.result()->setPipeline(pipeline.result()), IsOk());
    ASSERT_THAT(pass.result()->setBindGroup(0, group.result()), IsOk());
    ASSERT_THAT(pass.result()->dispatchWorkgroups(1, 1, 1), IsOk());
    ASSERT_THAT(pass.result()->end(), IsOk());
    ASSERT_THAT(encoder.result()->copyTextureToBuffer(
                    TexelCopyTextureInfo{output.result()}, readback.result(),
                    {0, kTransferRowBytes, kHeight}, {kWidth, kHeight}),
                IsOk());
    auto commands = encoder.result()->finish();
    ASSERT_THAT(commands, HasResult());
    auto serial = device.submit(std::move(commands).result());
    ASSERT_THAT(serial, HasResult());
    ASSERT_THAT(device.waitForSerial(serial.result(), 5.0), testing::IsTrue());
    const auto bytes = readbackBuffer(readback.result());
    ASSERT_THAT(bytes, HasResult());
    ASSERT_THAT(bytes.result(), testing::SizeIs(testing::Ge(kTransferRowBytes * kHeight)));

    auto actual = tiny_skia::filter::FloatPixmap::fromSize(kWidth, kHeight);
    auto expected = tiny_skia::filter::FloatPixmap::fromSize(kWidth, kHeight);
    ASSERT_THAT(actual.has_value(), testing::IsTrue());
    ASSERT_THAT(expected.has_value(), testing::IsTrue());
    for (uint32_t y = 0; y < kHeight; ++y) {
      std::memcpy(actual->data().data() + y * kWidth * 4,
                  bytes.result().data() + y * kTransferRowBytes, kPackedRowBytes);
      for (uint32_t x = 0; x < kWidth; ++x) {
        const size_t offset = (y * kWidth + x) * 4;
        const std::array<float, 4> mapSample{mapValues[offset], mapValues[offset + 1],
                                             mapValues[offset + 2], mapValues[offset + 3]};
        const float sampleX =
            static_cast<float>(x) +
            scenario.scale * (SelectStraightChannel(mapSample, scenario.xChannel) - 0.5f);
        const float sampleY =
            static_cast<float>(y) +
            scenario.scale * (SelectStraightChannel(mapSample, scenario.yChannel) - 0.5f);
        const std::array<float, 4> pixel = BilinearSample(sourceValues, sampleX, sampleY);
        std::copy(pixel.begin(), pixel.end(), expected->data().begin() + offset);
      }
    }
    ASSERT_THAT(actual->data(),
                testing::Each(testing::Truly([](float value) { return std::isfinite(value); })));
    const auto actualPixels = actual->toPixmap();
    const auto expectedPixels = expected->toPixmap();
    editor::tests::CompareBitmapToBitmap(
        svg::RendererBitmap{
            Vector2i(kWidth, kHeight),
            std::vector<uint8_t>(actualPixels.data().begin(), actualPixels.data().end()),
            kWidth * 4},
        svg::RendererBitmap{
            Vector2i(kWidth, kHeight),
            std::vector<uint8_t>(expectedPixels.data().begin(), expectedPixels.data().end()),
            kWidth * 4},
        std::string("displacement_") + scenario.name, editor::tests::PixelmatchIdentityParams());
  }
}

}  // namespace donner::gpu::tests
