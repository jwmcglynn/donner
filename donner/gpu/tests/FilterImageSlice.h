#pragma once
/// @file
/// Native image-filter sampling acceptance with an independent host reference.

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

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/shader/programs/FilterImageBindings.h"
#include "donner/gpu/tests/GpuTestUtils.h"

namespace donner::gpu::tests {

/// Host representation of the image filter's 40-byte uniform block.
struct FilterImageTestParams {
  float m00;
  float m01;
  float m02;
  float m10;
  float m11;
  float m12;
  uint32_t samplingMode;
  float pixelatedScaleX;
  float pixelatedScaleY;
  uint32_t padding;
};

static_assert(sizeof(FilterImageTestParams) == 40);

inline constexpr uint32_t kFilterImageWidth = 3;
inline constexpr uint32_t kFilterImageHeight = 2;
inline constexpr uint32_t kFilterImageOutputWidth = 7;
inline constexpr uint32_t kFilterImageOutputHeight = 5;
inline constexpr uint32_t kFilterImageBytesPerRow = 256;

/// A nonuniform premultiplied source with fractional alpha and one transparent border texel.
inline constexpr std::array<std::array<float, 4>, 6> kFilterImageTexels{{
    {0.92f, 0.10f, 0.05f, 1.0f},
    {0.10f, 0.42f, 0.08f, 0.5f},
    {0.0f, 0.0f, 0.0f, 0.0f},
    {0.05f, 0.02f, 0.18f, 0.25f},
    {0.15f, 0.15f, 0.70f, 0.75f},
    {0.33f, 0.12f, 0.42f, 0.6f},
}};

/// Samples one source channel with edge replication.
inline double FilterImageSource(int32_t x, int32_t y, size_t channel) {
  const int32_t sourceX = std::clamp(x, 0, static_cast<int32_t>(kFilterImageWidth) - 1);
  const int32_t sourceY = std::clamp(y, 0, static_cast<int32_t>(kFilterImageHeight) - 1);
  return kFilterImageTexels[sourceY * kFilterImageWidth + sourceX][channel];
}

/// Mitchell-Netravali reference polynomial with B=C=1/3.
inline double FilterImageCubicWeight(double input) {
  const double distance = std::abs(input);
  constexpr double b = 1.0 / 3.0;
  constexpr double c = 1.0 / 3.0;
  if (distance < 1.0) {
    return ((12.0 - 9.0 * b - 6.0 * c) * distance * distance * distance +
            (-18.0 + 12.0 * b + 6.0 * c) * distance * distance + 6.0 - 2.0 * b) /
           6.0;
  }
  if (distance < 2.0) {
    return ((-b - 6.0 * c) * distance * distance * distance +
            (6.0 * b + 30.0 * c) * distance * distance + (-12.0 * b - 48.0 * c) * distance +
            8.0 * b + 24.0 * c) /
           6.0;
  }
  return 0.0;
}

/// Independently evaluates one nearest-neighbor or pixelated channel.
inline double FilterImageSharpSample(double imageX, double imageY, size_t channel,
                                     const FilterImageTestParams& params) {
  if (params.samplingMode == 1) {
    return FilterImageSource(static_cast<int32_t>(std::floor(imageX + 0.5)),
                             static_cast<int32_t>(std::floor(imageY + 0.5)), channel);
  }

  const int32_t multipleX =
      std::clamp(static_cast<int32_t>(std::floor(params.pixelatedScaleX + 0.5f)), 1, 65536);
  const int32_t multipleY =
      std::clamp(static_cast<int32_t>(std::floor(params.pixelatedScaleY + 0.5f)), 1, 65536);
  const double virtualX = (imageX + 0.5) * multipleX - 0.5;
  const double virtualY = (imageY + 0.5) * multipleY - 0.5;
  const int32_t baseX = static_cast<int32_t>(std::floor(virtualX));
  const int32_t baseY = static_cast<int32_t>(std::floor(virtualY));
  const double fractionX = virtualX - baseX;
  const double fractionY = virtualY - baseY;
  const auto sample = [&](int32_t virtualSampleX, int32_t virtualSampleY) {
    const int32_t sourceX =
        static_cast<int32_t>(std::floor(static_cast<double>(virtualSampleX) / multipleX));
    const int32_t sourceY =
        static_cast<int32_t>(std::floor(static_cast<double>(virtualSampleY) / multipleY));
    return FilterImageSource(sourceX, sourceY, channel);
  };
  const double top = std::lerp(sample(baseX, baseY), sample(baseX + 1, baseY), fractionX);
  const double bottom =
      std::lerp(sample(baseX, baseY + 1), sample(baseX + 1, baseY + 1), fractionX);
  return std::lerp(top, bottom, fractionY);
}

/// Independently evaluates one output pixel, including transparent image bounds.
inline std::array<float, 4> FilterImageExpected(const FilterImageTestParams& params, int32_t x,
                                                int32_t y) {
  const double imageX = params.m00 * (x + 0.5) + params.m01 * (y + 0.5) + params.m02 - 0.5;
  const double imageY = params.m10 * (x + 0.5) + params.m11 * (y + 0.5) + params.m12 - 0.5;
  std::array<float, 4> result{};
  if (imageX < -0.5 || imageY < -0.5 || imageX >= kFilterImageWidth - 0.5 ||
      imageY >= kFilterImageHeight - 0.5) {
    return result;
  }

  for (size_t channel = 0; channel < result.size(); ++channel) {
    double value = 0.0;
    if (params.samplingMode != 0) {
      value = FilterImageSharpSample(imageX, imageY, channel, params);
    } else {
      const int32_t baseX = static_cast<int32_t>(std::floor(imageX));
      const int32_t baseY = static_cast<int32_t>(std::floor(imageY));
      for (int32_t yTap = -1; yTap <= 2; ++yTap) {
        double row = 0.0;
        for (int32_t xTap = -1; xTap <= 2; ++xTap) {
          row += FilterImageSource(baseX + xTap, baseY + yTap, channel) *
                 FilterImageCubicWeight(imageX - (baseX + xTap));
        }
        value += row * FilterImageCubicWeight(imageY - (baseY + yTap));
      }
    }
    result[channel] = static_cast<float>(std::clamp(value, 0.0, 1.0));
  }
  for (size_t channel = 0; channel < 3; ++channel) {
    result[channel] = std::min(result[channel], result[3]);
  }
  return result;
}

/// Affine placement cases spanning asymmetric scale, shear, rotation, crops, and transparent
/// borders. @param samplingMode Smooth (0), nearest (1), or pixelated (2).
inline std::array<FilterImageTestParams, 3> FilterImageScenes(uint32_t samplingMode) {
  return {{{0.48f, 0.13f, -0.20f, -0.08f, 0.62f, 0.35f, samplingMode, 2.6f, 1.6f, 0},
           {0.78f, -0.22f, 0.65f, 0.27f, 0.44f, -0.35f, samplingMode, 3.4f, 2.4f, 0},
           {0.40f, 0.0f, 1.20f, 0.0f, 0.60f, -0.90f, samplingMode, 4.2f, 1.2f, 0}}};
}

/// Executes one sampling mode and affine scene against a native backend descriptor.
/// @param device Native device with bounded wait/readback support.
/// @param shaderDescriptor Build-generated descriptor for the native backend.
/// @param readbackBuffer Reads a submitted buffer through the backend's host mapping API.
/// @param samplingMode Smooth (0), nearest (1), or pixelated (2).
/// @param sceneIndex Index into FilterImageScenes.
template <typename DeviceType, typename Readback>
void CheckFilterImageStorage(DeviceType& device, const ShaderModuleDescriptor& shaderDescriptor,
                             Readback readbackBuffer, uint32_t samplingMode, size_t sceneIndex) {
  const auto scenes = FilterImageScenes(samplingMode);
  ASSERT_THAT(sceneIndex, testing::Lt(scenes.size()));
  const FilterImageTestParams params = scenes[sceneIndex];
  const auto binding = [](shader::programs::FilterImageBinding value) {
    return static_cast<uint32_t>(value);
  };

  auto shaderModule = device.createShaderModule(shaderDescriptor);
  ASSERT_THAT(shaderModule, HasResult());
  auto bindGroupLayout = device.createBindGroupLayout(BindGroupLayoutDescriptor{
      "image filter",
      {{binding(shader::programs::FilterImageBinding::ImageTexture), ShaderStage::Compute,
        BindingType::SampledTexture2dUnfilterableFloat},
       {binding(shader::programs::FilterImageBinding::OutputTexture), ShaderStage::Compute,
        BindingType::WriteOnlyStorageTexture2d, TextureFormat::RGBA32Float},
       {binding(shader::programs::FilterImageBinding::Params), ShaderStage::Compute,
        BindingType::UniformBuffer}}});
  ASSERT_THAT(bindGroupLayout, HasResult());
  auto pipelineLayout = device.createPipelineLayout(
      PipelineLayoutDescriptor{"image filter", {bindGroupLayout.result()}});
  ASSERT_THAT(pipelineLayout, HasResult());
  auto pipeline = device.createComputePipeline(ComputePipelineDescriptor{
      "image filter",
      pipelineLayout.result(),
      ComputeState{shaderModule.result(), RcString(shader::programs::kFilterImageEntryPoint)},
      {shader::programs::kFilterImageWorkgroupSize, shader::programs::kFilterImageWorkgroupSize,
       1}});
  ASSERT_THAT(pipeline, HasResult());

  auto input =
      device.createTexture(TextureDescriptor{"image filter input",
                                             {kFilterImageWidth, kFilterImageHeight},
                                             TextureFormat::RGBA32Float,
                                             TextureUsage::Sampled | TextureUsage::CopyDst});
  auto output =
      device.createTexture(TextureDescriptor{"image filter output",
                                             {kFilterImageOutputWidth, kFilterImageOutputHeight},
                                             TextureFormat::RGBA32Float,
                                             TextureUsage::StorageBinding | TextureUsage::CopySrc});
  ASSERT_THAT(input, HasResult());
  ASSERT_THAT(output, HasResult());
  auto inputView = device.createTextureView(input.result(), TextureViewDescriptor{"input"});
  auto outputView = device.createTextureView(output.result(), TextureViewDescriptor{"output"});
  ASSERT_THAT(inputView, HasResult());
  ASSERT_THAT(outputView, HasResult());

  std::array<uint8_t, kFilterImageBytesPerRow * kFilterImageHeight> upload{};
  for (size_t y = 0; y < kFilterImageHeight; ++y) {
    std::memcpy(upload.data() + y * kFilterImageBytesPerRow,
                kFilterImageTexels.data() + y * kFilterImageWidth,
                kFilterImageWidth * sizeof(kFilterImageTexels.front()));
  }
  ASSERT_THAT(
      device.writeTexture(input.result(), upload, {0, kFilterImageBytesPerRow, kFilterImageHeight},
                          {kFilterImageWidth, kFilterImageHeight}),
      IsOk());

  auto uniform = device.createBuffer(BufferDescriptor{"image filter parameters", sizeof(params),
                                                      BufferUsage::Uniform | BufferUsage::CopyDst});
  ASSERT_THAT(uniform, HasResult());
  ASSERT_THAT(device.writeBuffer(uniform.result(), 0,
                                 std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&params),
                                                          sizeof(params))),
              IsOk());
  auto bindGroup = device.createBindGroup(
      BindGroupDescriptor{"image filter",
                          bindGroupLayout.result(),
                          {{binding(shader::programs::FilterImageBinding::ImageTexture),
                            TextureViewBinding{inputView.result()}},
                           {binding(shader::programs::FilterImageBinding::OutputTexture),
                            TextureViewBinding{outputView.result()}},
                           {binding(shader::programs::FilterImageBinding::Params),
                            BufferBinding{uniform.result(), 0, sizeof(params)}}}});
  ASSERT_THAT(bindGroup, HasResult());

  constexpr uint64_t kReadbackBytes = kFilterImageBytesPerRow * kFilterImageOutputHeight;
  auto readback = device.createBuffer(BufferDescriptor{
      "image filter readback", kReadbackBytes, BufferUsage::CopyDst | BufferUsage::MapRead});
  ASSERT_THAT(readback, HasResult());
  auto encoder = device.createCommandEncoder();
  ASSERT_THAT(encoder, HasResult());
  auto pass = encoder.result()->beginComputePass(ComputePassDescriptor{"image filter"});
  ASSERT_THAT(pass, HasResult());
  ASSERT_THAT(pass.result()->setPipeline(pipeline.result()), IsOk());
  ASSERT_THAT(pass.result()->setBindGroup(0, bindGroup.result()), IsOk());
  ASSERT_THAT(pass.result()->dispatchWorkgroups(1, 1, 1), IsOk());
  ASSERT_THAT(pass.result()->end(), IsOk());
  ASSERT_THAT(encoder.result()->copyTextureToBuffer(
                  TexelCopyTextureInfo{output.result()}, readback.result(),
                  {0, kFilterImageBytesPerRow, kFilterImageOutputHeight},
                  {kFilterImageOutputWidth, kFilterImageOutputHeight}),
              IsOk());
  auto commands = encoder.result()->finish();
  ASSERT_THAT(commands, HasResult());
  auto serial = device.submit(std::move(commands).result());
  ASSERT_THAT(serial, HasResult());
  ASSERT_THAT(device.waitForSerial(serial.result(), 5.0), testing::IsTrue());
  auto bytes = readbackBuffer(readback.result());
  ASSERT_THAT(bytes, HasResult());
  ASSERT_THAT(bytes.result(), testing::SizeIs(testing::Ge(kReadbackBytes)));

  svg::RendererBitmap actualBitmap;
  actualBitmap.dimensions = Vector2i(static_cast<int32_t>(kFilterImageOutputWidth),
                                     static_cast<int32_t>(kFilterImageOutputHeight));
  actualBitmap.rowBytes = kFilterImageOutputWidth * 4;
  actualBitmap.pixels.resize(actualBitmap.rowBytes * kFilterImageOutputHeight);
  svg::RendererBitmap expectedBitmap = actualBitmap;
  for (int32_t y = 0; y < static_cast<int32_t>(kFilterImageOutputHeight); ++y) {
    for (int32_t x = 0; x < static_cast<int32_t>(kFilterImageOutputWidth); ++x) {
      std::array<float, 4> actual{};
      std::memcpy(actual.data(),
                  bytes.result().data() + y * kFilterImageBytesPerRow + x * sizeof(actual),
                  sizeof(actual));
      EXPECT_THAT(actual,
                  testing::Each(testing::Truly([](float value) { return std::isfinite(value); })))
          << "pixel=" << x << "," << y;
      const std::array<float, 4> expected = FilterImageExpected(params, x, y);
      const size_t pixelOffset = (y * kFilterImageOutputWidth + x) * 4;
      for (size_t channel = 0; channel < 4; ++channel) {
        actualBitmap.pixels[pixelOffset + channel] = static_cast<uint8_t>(
            std::floor(std::clamp(actual[channel], 0.0f, 1.0f) * 255.0f + 0.5f));
        expectedBitmap.pixels[pixelOffset + channel] =
            static_cast<uint8_t>(std::floor(expected[channel] * 255.0f + 0.5f));
      }
    }
  }
  editor::tests::CompareBitmapToBitmap(
      actualBitmap, expectedBitmap,
      "native_image_mode_" + std::to_string(samplingMode) + "_scene_" + std::to_string(sceneIndex),
      editor::tests::PixelmatchIdentityParams());
}

}  // namespace donner::gpu::tests
