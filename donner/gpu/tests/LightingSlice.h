#pragma once
/// @file
/// Native diffuse/specular lighting acceptance over asymmetric height maps.

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
#include "donner/gpu/shader/programs/LightingBindings.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "tiny_skia/filter/FloatPixmap.h"

namespace donner::gpu::tests {
namespace lighting_detail {

inline constexpr int32_t kWidth = 5;
inline constexpr int32_t kHeight = 5;
inline constexpr uint32_t kBytesPerRow = 256;

struct Vec3 {
  float x;
  float y;
  float z;
};

inline Vec3 Subtract(Vec3 lhs, Vec3 rhs) {
  return {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

inline Vec3 Add(Vec3 lhs, Vec3 rhs) {
  return {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
}

inline float Dot(Vec3 lhs, Vec3 rhs) {
  return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

inline Vec3 Normalize(Vec3 value) {
  const float lengthSquared = Dot(value, value);
  if (!(lengthSquared > 0.0f)) {
    return {};
  }
  const float inverseLength = 1.0f / std::sqrt(lengthSquared);
  return {value.x * inverseLength, value.y * inverseLength, value.z * inverseLength};
}

inline float HeightAt(const std::array<float, kWidth * kHeight * 4>& input,
                      const shader::programs::LightingParams& params, int32_t x, int32_t y) {
  x = std::clamp(x, params.sampleMinX, params.sampleMaxX);
  y = std::clamp(y, params.sampleMinY, params.sampleMaxY);
  return input[(y * kWidth + x) * 4 + 3];
}

inline Vec3 ReferenceNormal(const std::array<float, kWidth * kHeight * 4>& input,
                            const shader::programs::LightingParams& params, int32_t x, int32_t y) {
  const auto horizontal = [&](int32_t sampleY) {
    if (x == params.sampleMinX) {
      return HeightAt(input, params, x + 1, sampleY) - HeightAt(input, params, x, sampleY);
    }
    if (x == params.sampleMaxX) {
      return HeightAt(input, params, x, sampleY) - HeightAt(input, params, x - 1, sampleY);
    }
    return HeightAt(input, params, x + 1, sampleY) - HeightAt(input, params, x - 1, sampleY);
  };
  const auto vertical = [&](int32_t sampleX) {
    if (y == params.sampleMinY) {
      return HeightAt(input, params, sampleX, y + 1) - HeightAt(input, params, sampleX, y);
    }
    if (y == params.sampleMaxY) {
      return HeightAt(input, params, sampleX, y) - HeightAt(input, params, sampleX, y - 1);
    }
    return HeightAt(input, params, sampleX, y + 1) - HeightAt(input, params, sampleX, y - 1);
  };

  float nx = 2.0f * horizontal(y);
  if (y > params.sampleMinY) nx += horizontal(y - 1);
  if (y < params.sampleMaxY) nx += horizontal(y + 1);
  float ny = 2.0f * vertical(x);
  if (x > params.sampleMinX) ny += vertical(x - 1);
  if (x < params.sampleMaxX) ny += vertical(x + 1);
  const bool corner = (x == params.sampleMinX || x == params.sampleMaxX) &&
                      (y == params.sampleMinY || y == params.sampleMaxY);
  const float divisor = corner ? 3.0f : 4.0f;
  return Normalize(
      {-params.surfaceScale * nx / divisor, -params.surfaceScale * ny / divisor, 1.0f});
}

inline Vec3 LightDirection(const shader::programs::LightingParams& params, int32_t x, int32_t y,
                           float surfaceZ) {
  if (params.lightType == 0) {
    const float cosElevation = std::cos(params.elevationRad);
    return Normalize({std::cos(params.azimuthRad) * cosElevation,
                      std::sin(params.azimuthRad) * cosElevation, std::sin(params.elevationRad)});
  }
  return Normalize(Subtract({params.lightX, params.lightY, params.lightZ},
                            {static_cast<float>(x), static_cast<float>(y), surfaceZ}));
}

inline float SpotFactor(const shader::programs::LightingParams& params, int32_t x, int32_t y,
                        float alpha, Vec3 lightDirection) {
  if (params.lightType != 2) {
    return 1.0f;
  }
  const Vec3 deviceSpotDirection =
      Normalize(Subtract({params.pointsAtX, params.pointsAtY, params.pointsAtZ},
                         {params.lightX, params.lightY, params.lightZ}));
  const float deviceCosine =
      Dot({-lightDirection.x, -lightDirection.y, -lightDirection.z}, deviceSpotDirection);
  if (deviceCosine <= 0.0f) {
    return 0.0f;
  }
  float cosine = deviceCosine;
  if (params.hasShear != 0) {
    const float userX = params.pixelToUser0 * x + params.pixelToUser1 * y + params.pixelToUser2;
    const float userY = params.pixelToUser3 * x + params.pixelToUser4 * y + params.pixelToUser5;
    const Vec3 lightToSurface = Normalize({userX - params.userLightX, userY - params.userLightY,
                                           params.surfaceScale * alpha - params.userLightZ});
    const Vec3 spotDirection = Normalize({params.userPointsAtX - params.userLightX,
                                          params.userPointsAtY - params.userLightY,
                                          params.userPointsAtZ - params.userLightZ});
    cosine = Dot(lightToSurface, spotDirection);
    if (cosine <= 0.0f) {
      return 0.0f;
    }
  }
  float coneFactor = 1.0f;
  if (params.hasConeAngle != 0) {
    const float outer = std::cos(params.coneAngleRad);
    if (cosine < outer) {
      return 0.0f;
    }
    if (cosine < outer + 0.016f) {
      coneFactor = (cosine - outer) / 0.016f;
    }
  }
  const float exponent = params.spotExponent > 0.0f ? params.spotExponent : 1.0f;
  return std::pow(cosine, exponent) * coneFactor;
}

inline std::array<float, 4> ReferencePixel(const std::array<float, kWidth * kHeight * 4>& input,
                                           const shader::programs::LightingParams& params,
                                           int32_t x, int32_t y, bool specular) {
  if (x < params.sampleMinX || x > params.sampleMaxX || y < params.sampleMinY ||
      y > params.sampleMaxY) {
    return {};
  }
  const float alpha = HeightAt(input, params, x, y);
  const Vec3 normal = ReferenceNormal(input, params, x, y);
  const Vec3 light = LightDirection(params, x, y, params.surfaceScale * alpha);
  float response = std::max(Dot(normal, light), 0.0f);
  if (specular) {
    response = std::max(Dot(normal, Normalize(Add(light, {0.0f, 0.0f, 1.0f}))), 0.0f);
    response = params.specularExponent == 0.0f ? 1.0f : std::pow(response, params.specularExponent);
  }
  const float intensity =
      params.lightingConstant * response * SpotFactor(params, x, y, alpha, light);
  const float red = std::clamp(intensity * params.lightR, 0.0f, 1.0f);
  const float green = std::clamp(intensity * params.lightG, 0.0f, 1.0f);
  const float blue = std::clamp(intensity * params.lightB, 0.0f, 1.0f);
  return {red, green, blue, specular ? std::max({red, green, blue}) : 1.0f};
}

inline shader::programs::LightingParams MakeParams(bool specular, uint32_t lightType) {
  shader::programs::LightingParams params;
  params.surfaceScale = 2.75f;
  params.lightingConstant = specular ? 0.93f : 0.81f;
  params.specularExponent = 7.0f;
  params.lightR = 0.82f;
  params.lightG = 0.53f;
  params.lightB = 0.29f;
  params.lightType = lightType;
  params.azimuthRad = 0.61f;
  params.elevationRad = 0.83f;
  params.lightX = -1.2f;
  params.lightY = 1.4f;
  params.lightZ = 4.6f;
  params.userLightX = -1.7f;
  params.userLightY = 0.9f;
  params.userLightZ = 4.2f;
  params.pointsAtX = 2.8f;
  params.pointsAtY = 2.1f;
  params.pointsAtZ = 0.4f;
  params.spotExponent = 2.25f;
  params.userPointsAtX = 2.4f;
  params.userPointsAtY = 2.8f;
  params.userPointsAtZ = 0.2f;
  params.coneAngleRad = 1.25f;
  params.pixelToUser0 = 1.0f;
  params.pixelToUser1 = 0.2f;
  params.pixelToUser2 = -0.4f;
  params.pixelToUser3 = -0.1f;
  params.pixelToUser4 = 0.9f;
  params.pixelToUser5 = 0.3f;
  params.hasShear = lightType == 2 ? 1 : 0;
  params.hasConeAngle = lightType == 2 ? 1 : 0;
  params.sampleMinX = 1;
  params.sampleMinY = 1;
  params.sampleMaxX = 3;
  params.sampleMaxY = 3;
  return params;
}

}  // namespace lighting_detail

/// Runs one lighting model/light-source pair and compares every finite output pixel strictly.
/// @param device Native device with bounded wait/readback support.
/// @param shaderDescriptor Build-time backend artifact with the shared compute entry point.
/// @param readbackBuffer Reads the submitted buffer through the backend's host mapping API.
/// @param specular Selects the specular output contract instead of diffuse.
/// @param lightType Zero distant, one point, two spot.
template <typename DeviceType, typename Readback>
void CheckLightingStorage(DeviceType& device, const ShaderModuleDescriptor& shaderDescriptor,
                          Readback readbackBuffer, bool specular, uint32_t lightType) {
  using namespace lighting_detail;
  using shader::programs::LightingBinding;
  const auto binding = [](LightingBinding value) { return static_cast<uint32_t>(value); };
  auto shaderModule = device.createShaderModule(shaderDescriptor);
  ASSERT_THAT(shaderModule, HasResult());
  auto layout = device.createBindGroupLayout(BindGroupLayoutDescriptor{
      "lighting",
      {{binding(LightingBinding::InputTexture), ShaderStage::Compute,
        BindingType::SampledTexture2dUnfilterableFloat},
       {binding(LightingBinding::OutputTexture), ShaderStage::Compute,
        BindingType::WriteOnlyStorageTexture2d, TextureFormat::RGBA32Float},
       {binding(LightingBinding::Params), ShaderStage::Compute,
        BindingType::ReadOnlyStorageBuffer}}});
  ASSERT_THAT(layout, HasResult());
  auto pipelineLayout =
      device.createPipelineLayout(PipelineLayoutDescriptor{"lighting", {layout.result()}});
  ASSERT_THAT(pipelineLayout, HasResult());
  auto pipeline = device.createComputePipeline(ComputePipelineDescriptor{
      "lighting",
      pipelineLayout.result(),
      ComputeState{shaderModule.result(), RcString(shader::programs::kLightingEntryPoint)},
      {shader::programs::kLightingWorkgroupSize, shader::programs::kLightingWorkgroupSize, 1}});
  ASSERT_THAT(pipeline, HasResult());
  auto input =
      device.createTexture(TextureDescriptor{"lighting input",
                                             {kWidth, kHeight},
                                             TextureFormat::RGBA32Float,
                                             TextureUsage::Sampled | TextureUsage::CopyDst});
  auto output =
      device.createTexture(TextureDescriptor{"lighting output",
                                             {kWidth, kHeight},
                                             TextureFormat::RGBA32Float,
                                             TextureUsage::StorageBinding | TextureUsage::CopySrc});
  ASSERT_THAT(input, HasResult());
  ASSERT_THAT(output, HasResult());
  auto inputView = device.createTextureView(input.result(), TextureViewDescriptor{"input"});
  auto outputView = device.createTextureView(output.result(), TextureViewDescriptor{"output"});
  ASSERT_THAT(inputView, HasResult());
  ASSERT_THAT(outputView, HasResult());

  std::array<float, kWidth * kHeight * 4> inputValues{};
  std::array<uint8_t, kHeight * kBytesPerRow> upload{};
  for (int32_t y = 0; y < kHeight; ++y) {
    for (int32_t x = 0; x < kWidth; ++x) {
      const size_t offset = (y * kWidth + x) * 4;
      inputValues[offset] = static_cast<float>(x + 1) / 7.0f;
      inputValues[offset + 1] = static_cast<float>(y + 2) / 8.0f;
      inputValues[offset + 2] = static_cast<float>(x + y + 3) / 11.0f;
      inputValues[offset + 3] = static_cast<float>((x * 3 + y * 5 + 1) % 9) / 8.0f;
    }
    std::memcpy(upload.data() + y * kBytesPerRow, inputValues.data() + y * kWidth * 4,
                kWidth * 4 * sizeof(float));
  }
  ASSERT_THAT(
      device.writeTexture(input.result(), upload, {0, kBytesPerRow, kHeight}, {kWidth, kHeight}),
      IsOk());

  const shader::programs::LightingParams params = MakeParams(specular, lightType);
  auto paramsBuffer = device.createBuffer(BufferDescriptor{
      "lighting parameters", sizeof(params), BufferUsage::Storage | BufferUsage::CopyDst});
  ASSERT_THAT(paramsBuffer, HasResult());
  ASSERT_THAT(device.writeBuffer(paramsBuffer.result(), 0,
                                 std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&params),
                                                          sizeof(params))),
              IsOk());
  auto group = device.createBindGroup(BindGroupDescriptor{
      "lighting",
      layout.result(),
      {{binding(LightingBinding::InputTexture), TextureViewBinding{inputView.result()}},
       {binding(LightingBinding::OutputTexture), TextureViewBinding{outputView.result()}},
       {binding(LightingBinding::Params),
        BufferBinding{paramsBuffer.result(), 0, sizeof(params)}}}});
  ASSERT_THAT(group, HasResult());
  auto readback = device.createBuffer(BufferDescriptor{
      "lighting readback", kHeight * kBytesPerRow, BufferUsage::CopyDst | BufferUsage::MapRead});
  ASSERT_THAT(readback, HasResult());
  auto encoder = device.createCommandEncoder();
  ASSERT_THAT(encoder, HasResult());
  auto pass = encoder.result()->beginComputePass(ComputePassDescriptor{"lighting"});
  ASSERT_THAT(pass, HasResult());
  ASSERT_THAT(pass.result()->setPipeline(pipeline.result()), IsOk());
  ASSERT_THAT(pass.result()->setBindGroup(0, group.result()), IsOk());
  ASSERT_THAT(pass.result()->dispatchWorkgroups(1, 1, 1), IsOk());
  ASSERT_THAT(pass.result()->end(), IsOk());
  ASSERT_THAT(encoder.result()->copyTextureToBuffer(TexelCopyTextureInfo{output.result()},
                                                    readback.result(), {0, kBytesPerRow, kHeight},
                                                    {kWidth, kHeight}),
              IsOk());
  auto commands = encoder.result()->finish();
  ASSERT_THAT(commands, HasResult());
  auto serial = device.submit(std::move(commands).result());
  ASSERT_THAT(serial, HasResult());
  ASSERT_THAT(device.waitForSerial(serial.result(), 5.0), testing::IsTrue());
  auto bytes = readbackBuffer(readback.result());
  ASSERT_THAT(bytes, HasResult());
  ASSERT_THAT(bytes.result(), testing::SizeIs(testing::Ge(kHeight * kBytesPerRow)));

  auto actual = tiny_skia::filter::FloatPixmap::fromSize(kWidth, kHeight);
  auto expected = tiny_skia::filter::FloatPixmap::fromSize(kWidth, kHeight);
  ASSERT_THAT(actual.has_value(), testing::IsTrue());
  ASSERT_THAT(expected.has_value(), testing::IsTrue());
  for (int32_t y = 0; y < kHeight; ++y) {
    std::memcpy(actual->data().data() + y * kWidth * 4, bytes.result().data() + y * kBytesPerRow,
                kWidth * 4 * sizeof(float));
    for (int32_t x = 0; x < kWidth; ++x) {
      const std::array<float, 4> reference = ReferencePixel(inputValues, params, x, y, specular);
      std::copy(reference.begin(), reference.end(),
                expected->data().begin() + (y * kWidth + x) * 4);
    }
  }
  ASSERT_THAT(actual->data(),
              testing::Each(testing::Truly([](float value) { return std::isfinite(value); })));
  const auto actualPixels = actual->toPixmap();
  const auto expectedPixels = expected->toPixmap();
  editor::tests::CompareBitmapToBitmap(
      svg::RendererBitmap{
          Vector2i(kWidth, kHeight),
          std::vector<uint8_t>(actualPixels.data().begin(), actualPixels.data().end()), kWidth * 4},
      svg::RendererBitmap{
          Vector2i(kWidth, kHeight),
          std::vector<uint8_t>(expectedPixels.data().begin(), expectedPixels.data().end()),
          kWidth * 4},
      std::string(specular ? "specular" : "diffuse") + "_light_" + std::to_string(lightType),
      editor::tests::PixelmatchIdentityParams());
}

}  // namespace donner::gpu::tests
