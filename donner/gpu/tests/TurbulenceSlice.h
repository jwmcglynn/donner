#pragma once
/// @file
/// Native turbulence seed, octave, stitching, transform, and pixel acceptance.

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
#include <vector>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/shader/programs/TurbulenceBindings.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "tiny_skia/filter/Turbulence.h"

namespace donner::gpu::tests {
namespace turbulence_details {

inline constexpr uint32_t kWidth = 7;
inline constexpr uint32_t kHeight = 5;
inline constexpr uint32_t kTransferRowBytes = 256;
inline constexpr uint32_t kPackedRowBytes = kWidth * 4 * sizeof(float);

struct Params {
  float baseFreqX;
  float baseFreqY;
  int32_t numOctaves;
  int32_t seed;
  uint32_t stitchTiles;
  uint32_t typeFlag;
  float tileWidth;
  float tileHeight;
  float filterFromDeviceA;
  float filterFromDeviceB;
  float filterFromDeviceC;
  float filterFromDeviceD;
};

struct Tables {
  std::array<int32_t, shader::programs::kTurbulenceTableSize> lattice{};
  std::array<float, shader::programs::kTurbulenceGradientTableSize> gradX{};
  std::array<float, shader::programs::kTurbulenceGradientTableSize> gradY{};
};

static_assert(sizeof(Params) == 48);
static_assert(sizeof(Tables) == 18504);

inline int64_t Random(int64_t seed) {
  constexpr int64_t kRandM = 2147483647;
  constexpr int64_t kRandA = 16807;
  constexpr int64_t kRandQ = 127773;
  constexpr int64_t kRandR = 2836;
  int64_t result = kRandA * (seed % kRandQ) - kRandR * (seed / kRandQ);
  if (result <= 0) {
    result += kRandM;
  }
  return result;
}

inline Tables BuildTables(double seedValue) {
  constexpr int64_t kRandM = 2147483647;
  constexpr int32_t kBase = shader::programs::kTurbulenceBaseTableSize;
  constexpr int32_t kTable = shader::programs::kTurbulenceTableSize;
  int64_t seed = seedValue <= 0 ? -static_cast<int64_t>(seedValue) % (kRandM - 1) + 1
                                : std::min(static_cast<int64_t>(seedValue), kRandM - 1);
  Tables tables;
  double gradient[4][kTable][2]{};
  for (int32_t channel = 0; channel < 4; ++channel) {
    for (int32_t index = 0; index < kBase; ++index) {
      if (channel == 0) {
        tables.lattice[index] = index;
      }
      seed = Random(seed);
      gradient[channel][index][0] = static_cast<double>((seed % (2 * kBase)) - kBase) / kBase;
      seed = Random(seed);
      gradient[channel][index][1] = static_cast<double>((seed % (2 * kBase)) - kBase) / kBase;
      const double length = std::sqrt(gradient[channel][index][0] * gradient[channel][index][0] +
                                      gradient[channel][index][1] * gradient[channel][index][1]);
      if (length > 1e-10) {
        gradient[channel][index][0] /= length;
        gradient[channel][index][1] /= length;
      }
    }
  }
  for (int32_t index = kBase - 1; index > 0; --index) {
    seed = Random(seed);
    const int32_t target = static_cast<int32_t>(seed % kBase);
    std::swap(tables.lattice[index], tables.lattice[target]);
  }
  for (int32_t index = 0; index < kBase + 2; ++index) {
    tables.lattice[kBase + index] = tables.lattice[index];
    for (int32_t channel = 0; channel < 4; ++channel) {
      gradient[channel][kBase + index][0] = gradient[channel][index][0];
      gradient[channel][kBase + index][1] = gradient[channel][index][1];
    }
  }
  for (int32_t channel = 0; channel < 4; ++channel) {
    for (int32_t index = 0; index < kTable; ++index) {
      const size_t offset = channel * kTable + index;
      tables.gradX[offset] = static_cast<float>(gradient[channel][index][0]);
      tables.gradY[offset] = static_cast<float>(gradient[channel][index][1]);
    }
  }
  return tables;
}

struct Scenario {
  const char* name;
  tiny_skia::filter::TurbulenceType type;
  double baseFrequencyX;
  double baseFrequencyY;
  int numOctaves;
  double seed;
  bool stitchTiles;
  std::array<double, 4> filterFromDevice;
};

inline const std::array<Scenario, 4> kScenarios{{
    {"turbulence_seed_1_octave_1",
     tiny_skia::filter::TurbulenceType::Turbulence,
     0.125,
     0.2,
     1,
     1,
     false,
     {1, 0, 0, 1}},
    {"fractal_seed_13_octaves_3",
     tiny_skia::filter::TurbulenceType::FractalNoise,
     0.07,
     0.11,
     3,
     13,
     false,
     {0.75, 0.2, -0.1, 1.25}},
    {"turbulence_negative_seed_stitched",
     tiny_skia::filter::TurbulenceType::Turbulence,
     0.13,
     0.09,
     4,
     -7.5,
     true,
     {1, 0, 0, 1}},
    {"fractal_asymmetric_stitched",
     tiny_skia::filter::TurbulenceType::FractalNoise,
     0.03125,
     0.1875,
     2,
     42,
     true,
     {1.25, -0.25, 0.125, 0.75}},
}};

}  // namespace turbulence_details

/**
 * Runs build-generated turbulence code against the independent CPU filter.
 *
 * @param device Native device with bounded wait/readback support.
 * @param shaderDescriptor Build-generated platform descriptor.
 * @param readbackBuffer Reads a submitted buffer through the backend's host mapping API.
 */
template <typename DeviceType, typename Readback>
void CheckTurbulenceStorage(DeviceType& device, const ShaderModuleDescriptor& shaderDescriptor,
                            Readback readbackBuffer) {
  using namespace turbulence_details;
  using shader::programs::TurbulenceBinding;
  const auto binding = [](TurbulenceBinding value) { return static_cast<uint32_t>(value); };

  auto shader = device.createShaderModule(shaderDescriptor);
  ASSERT_THAT(shader, HasResult());
  auto layout = device.createBindGroupLayout(BindGroupLayoutDescriptor{
      "turbulence",
      {{binding(TurbulenceBinding::OutputTexture), ShaderStage::Compute,
        BindingType::WriteOnlyStorageTexture2d, TextureFormat::RGBA32Float},
       {binding(TurbulenceBinding::Params), ShaderStage::Compute,
        BindingType::ReadOnlyStorageBuffer},
       {binding(TurbulenceBinding::Tables), ShaderStage::Compute,
        BindingType::ReadOnlyStorageBuffer}}});
  ASSERT_THAT(layout, HasResult());
  auto pipelineLayout =
      device.createPipelineLayout(PipelineLayoutDescriptor{"turbulence", {layout.result()}});
  ASSERT_THAT(pipelineLayout, HasResult());
  auto pipeline = device.createComputePipeline(ComputePipelineDescriptor{
      "turbulence",
      pipelineLayout.result(),
      ComputeState{shader.result(), RcString(shader::programs::kTurbulenceEntryPoint)},
      {shader::programs::kTurbulenceWorkgroupSize, shader::programs::kTurbulenceWorkgroupSize, 1}});
  ASSERT_THAT(pipeline, HasResult());

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

    const Params params{static_cast<float>(scenario.baseFrequencyX),
                        static_cast<float>(scenario.baseFrequencyY),
                        scenario.numOctaves,
                        static_cast<int32_t>(std::round(scenario.seed)),
                        scenario.stitchTiles ? 1u : 0u,
                        scenario.type == tiny_skia::filter::TurbulenceType::Turbulence ? 1u : 0u,
                        static_cast<float>(kWidth),
                        static_cast<float>(kHeight),
                        static_cast<float>(scenario.filterFromDevice[0]),
                        static_cast<float>(scenario.filterFromDevice[1]),
                        static_cast<float>(scenario.filterFromDevice[2]),
                        static_cast<float>(scenario.filterFromDevice[3])};
    const Tables tables = BuildTables(scenario.seed);
    auto paramsBuffer = device.createBuffer(BufferDescriptor{
        "turbulence params", sizeof(params), BufferUsage::Storage | BufferUsage::CopyDst});
    auto tablesBuffer = device.createBuffer(BufferDescriptor{
        "turbulence tables", sizeof(tables), BufferUsage::Storage | BufferUsage::CopyDst});
    ASSERT_THAT(paramsBuffer, HasResult());
    ASSERT_THAT(tablesBuffer, HasResult());
    ASSERT_THAT(device.writeBuffer(paramsBuffer.result(), 0,
                                   std::span<const uint8_t>(
                                       reinterpret_cast<const uint8_t*>(&params), sizeof(params))),
                IsOk());
    ASSERT_THAT(device.writeBuffer(tablesBuffer.result(), 0,
                                   std::span<const uint8_t>(
                                       reinterpret_cast<const uint8_t*>(&tables), sizeof(tables))),
                IsOk());
    auto group = device.createBindGroup(BindGroupDescriptor{
        scenario.name,
        layout.result(),
        {{binding(TurbulenceBinding::OutputTexture), TextureViewBinding{outputView.result()}},
         {binding(TurbulenceBinding::Params),
          BufferBinding{paramsBuffer.result(), 0, sizeof(params)}},
         {binding(TurbulenceBinding::Tables),
          BufferBinding{tablesBuffer.result(), 0, sizeof(tables)}}}});
    ASSERT_THAT(group, HasResult());
    auto readback =
        device.createBuffer(BufferDescriptor{"turbulence readback", kTransferRowBytes * kHeight,
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
    }
    tiny_skia::filter::TurbulenceParams cpuParams;
    cpuParams.type = scenario.type;
    cpuParams.baseFrequencyX = scenario.baseFrequencyX;
    cpuParams.baseFrequencyY = scenario.baseFrequencyY;
    cpuParams.numOctaves = scenario.numOctaves;
    cpuParams.seed = scenario.seed;
    cpuParams.stitchTiles = scenario.stitchTiles;
    cpuParams.tileWidth = kWidth;
    cpuParams.tileHeight = kHeight;
    cpuParams.filterFromDeviceA = scenario.filterFromDevice[0];
    cpuParams.filterFromDeviceB = scenario.filterFromDevice[1];
    cpuParams.filterFromDeviceC = scenario.filterFromDevice[2];
    cpuParams.filterFromDeviceD = scenario.filterFromDevice[3];
    tiny_skia::filter::turbulence(*expected, cpuParams);

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
        std::string("turbulence_") + scenario.name, editor::tests::PixelmatchIdentityParams());
  }
}

}  // namespace donner::gpu::tests
