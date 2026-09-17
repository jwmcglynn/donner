#pragma once
/// @file
/// Native component-transfer function, boundary, and packed-table acceptance.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/gpu/tests/ReflectedComputeSlice.h"
#include "tiny_skia/filter/GaussianBlur.h"

namespace donner::gpu::tests {
namespace component_transfer {

/// One channel function in the SVG component-transfer model.
struct Function {
  uint32_t kind = 0;
  std::vector<float> table;
  float slope = 1;
  float intercept = 0;
  float amplitude = 1;
  float exponent = 1;
  float offset = 0;
};

/// One independently named native acceptance case.
struct Case {
  std::string_view name;
  std::array<Function, 4> functions;
  /// Measured GPU-vs-oracle LSB ties this case may carry; 0 requires identity.
  int allowedMismatchedPixels = 0;
};

/// Packs four eight-float records followed by a bounded concatenated table.
inline std::vector<float> Pack(const std::array<Function, 4>& functions) {
  size_t tableCount = 0;
  for (const Function& function : functions) {
    tableCount += function.table.size();
  }
  std::vector<float> result(32 + std::max<size_t>(tableCount, 1));
  size_t tableOffset = 0;
  for (size_t channel = 0; channel < functions.size(); ++channel) {
    const Function& function = functions[channel];
    const size_t base = channel * 8;
    result[base] = static_cast<float>(function.kind);
    result[base + 1] = static_cast<float>(tableOffset);
    result[base + 2] = static_cast<float>(function.table.size());
    result[base + 3] = function.slope;
    result[base + 4] = function.intercept;
    result[base + 5] = function.amplitude;
    result[base + 6] = function.exponent;
    result[base + 7] = function.offset;
    std::copy(function.table.begin(), function.table.end(), result.begin() + 32 + tableOffset);
    tableOffset += function.table.size();
  }
  return result;
}

/// Evaluates the SVG function independently from the shader implementation.
inline float Evaluate(const Function& function, float value) {
  const float c = std::clamp(value, 0.0f, 1.0f);
  float result = c;
  switch (function.kind) {
    case 1:
      if (function.table.size() == 1) {
        result = function.table.front();
      } else if (function.table.size() > 1) {
        const float position = c * static_cast<float>(function.table.size() - 1);
        const size_t index = std::min(static_cast<size_t>(position), function.table.size() - 2);
        const float fraction = position - static_cast<float>(index);
        result = function.table[index] * (1 - fraction) + function.table[index + 1] * fraction;
      }
      break;
    case 2:
      if (!function.table.empty()) {
        const size_t index =
            std::min(static_cast<size_t>(c * static_cast<float>(function.table.size())),
                     function.table.size() - 1);
        result = function.table[index];
      }
      break;
    case 3: result = function.slope * c + function.intercept; break;
    case 4:
      if (function.amplitude == 0) {
        result = function.offset;
      } else if (function.exponent == 0) {
        result = function.amplitude + function.offset;
      } else if (c == 0 && function.exponent < 0) {
        result = function.amplitude > 0 ? 1.0f : 0.0f;
      } else {
        result = function.amplitude * std::pow(c, function.exponent) + function.offset;
      }
      break;
    default: break;
  }
  return std::clamp(result, 0.0f, 1.0f);
}

/// Varied premultiplied input containing transparent, boundary, and endpoint values.
inline std::array<float, 64> InputValues() {
  std::array<float, 64> values{};
  for (size_t index = 0; index < 16; ++index) {
    const float alpha = index == 0 ? 0.0f : float(index % 4 + 1) / 4;
    const std::array<float, 3> straight{float(index % 5) / 4, float(index % 7) / 6,
                                        float((index * 3) % 5) / 4};
    for (size_t channel = 0; channel < 3; ++channel) {
      values[index * 4 + channel] = straight[channel] * alpha;
    }
    values[index * 4 + 3] = alpha;
  }
  return values;
}

/// Mixed Table, Discrete, Linear, and identity functions.
inline Case MixedFunctions() {
  Case result{"mixed_functions", {}};
  result.functions[0] = Function{.kind = 1, .table = {0, 0.25f, 1}};
  result.functions[1] = Function{.kind = 2, .table = {0.125f, 0.5f, 0.875f}};
  result.functions[2] = Function{.kind = 3, .slope = 0.5f, .intercept = 0.25f};
  return result;
}

/// Ordinary gamma, negative-exponent zero input, and exponent-zero behavior.
inline Case GammaFunctions() {
  // The reciprocal-exponent channel lands three pixels on an exact 127.5/255
  // boundary on Metal: the GPU's `pow(x, -1)` rounds one ULP below the CPU
  // oracle, so the 8-bit conversion gives 127 where the oracle gives 128 (RGB
  // bit-identical elsewhere, blue off by one LSB). Pixelmatch 1.x masked this
  // through uint8 blend quantization; 2.0 compares at full precision
  // (PR #1285), so this case carries the measured three-pixel allowance and
  // every other pixel stays bit-exact.
  Case result{"gamma_functions", {}, /*allowedMismatchedPixels=*/3};
  result.functions[0] = Function{.kind = 4, .amplitude = 0.5f, .exponent = 2, .offset = 0.125f};
  result.functions[1] = Function{.kind = 3, .slope = 0.75f, .intercept = 0.125f};
  result.functions[2] = Function{.kind = 4, .amplitude = 0.5f, .exponent = -1};
  result.functions[3] = Function{.kind = 4, .amplitude = 0.5f, .exponent = 0, .offset = 0.25f};
  return result;
}

/// Empty and singleton tables, zero amplitude, and unknown-kind identity.
inline Case GuardAndBoundaryFunctions() {
  Case result{"guard_and_boundary_functions", {}};
  result.functions[0] = Function{.kind = 4, .amplitude = 0, .exponent = 2, .offset = 0.25f};
  result.functions[1] = Function{.kind = 1};
  result.functions[2] = Function{.kind = 1, .table = {0.625f}};
  result.functions[3] = Function{.kind = 255};
  return result;
}

/// Four maximum-sized nonconstant tables with distinct within-table patterns.
inline Case MaximumTables() {
  Case result{"maximum_tables", {}};
  for (size_t channel = 0; channel < result.functions.size(); ++channel) {
    Function& function = result.functions[channel];
    function.kind = 1;
    function.table.resize(1024);
    for (size_t index = 0; index < function.table.size(); ++index) {
      function.table[index] = float((index + channel * 3) % 8) / 8;
    }
  }
  return result;
}

}  // namespace component_transfer

/// Checks each function family and packed-table boundary through a native backend.
/// @param device Native device with bounded wait/readback support.
/// @param shader Selected or mutation artifact; bindings and workgroup come from reflection.
/// @param readbackBuffer Reads the submitted buffer through the backend's host mapping API.
template <typename DeviceType, typename Readback>
void CheckComponentTransferStorage(DeviceType& device, const shader::CompiledShaderView& shader,
                                   Readback readbackBuffer) {
  ReflectedComputePipeline compute;
  CreateReflectedComputePipeline(device, shader, "component transfer", compute);
  if (testing::Test::HasFatalFailure()) return;

  const std::array<component_transfer::Case, 4> cases{
      component_transfer::MixedFunctions(), component_transfer::GammaFunctions(),
      component_transfer::GuardAndBoundaryFunctions(), component_transfer::MaximumTables()};
  const std::array<float, 64> inputValues = component_transfer::InputValues();
  for (const component_transfer::Case& test : cases) {
    SCOPED_TRACE(test.name);
    auto input =
        device.createTexture(TextureDescriptor{"component transfer input",
                                               {4, 4},
                                               TextureFormat::RGBA32Float,
                                               TextureUsage::Sampled | TextureUsage::CopyDst});
    auto output = device.createTexture(
        TextureDescriptor{"component transfer output",
                          {4, 4},
                          TextureFormat::RGBA32Float,
                          TextureUsage::StorageBinding | TextureUsage::CopySrc});
    ASSERT_THAT(input, HasResult());
    ASSERT_THAT(output, HasResult());
    auto inputView = device.createTextureView(input.result(), TextureViewDescriptor{"input"});
    auto outputView = device.createTextureView(output.result(), TextureViewDescriptor{"output"});
    ASSERT_THAT(inputView, HasResult());
    ASSERT_THAT(outputView, HasResult());

    std::array<uint8_t, 1024> upload{};
    for (size_t y = 0; y < 4; ++y) {
      std::memcpy(upload.data() + y * 256, inputValues.data() + y * 16, 16 * sizeof(float));
    }
    ASSERT_THAT(device.writeTexture(input.result(), upload, {0, 256, 4}, {4, 4}), IsOk());
    const std::vector<float> params = component_transfer::Pack(test.functions);
    auto parameterBuffer = device.createBuffer(
        BufferDescriptor{"component transfer parameters", params.size() * sizeof(float),
                         BufferUsage::Storage | BufferUsage::CopyDst});
    ASSERT_THAT(parameterBuffer, HasResult());
    ASSERT_THAT(
        device.writeBuffer(parameterBuffer.result(), 0,
                           std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(params.data()),
                                                    params.size() * sizeof(float))),
        IsOk());
    auto group = device.createBindGroup(BindGroupDescriptor{
        "component transfer",
        compute.layout,
        {{ReflectedBinding(shader, "inputTexture"), TextureViewBinding{inputView.result()}},
         {ReflectedBinding(shader, "outputTexture"), TextureViewBinding{outputView.result()}},
         {ReflectedBinding(shader, "params"),
          BufferBinding{parameterBuffer.result(), 0, params.size() * sizeof(float)}}}});
    ASSERT_THAT(group, HasResult());
    auto readback = device.createBuffer(BufferDescriptor{
        "component transfer readback", 1024, BufferUsage::CopyDst | BufferUsage::MapRead});
    ASSERT_THAT(readback, HasResult());
    auto encoder = device.createCommandEncoder();
    ASSERT_THAT(encoder, HasResult());
    auto pass = encoder.result()->beginComputePass(ComputePassDescriptor{"component transfer"});
    ASSERT_THAT(pass, HasResult());
    ASSERT_THAT(pass.result()->setPipeline(compute.pipeline), IsOk());
    ASSERT_THAT(pass.result()->setBindGroup(0, group.result()), IsOk());
    const auto groups = compute.groupsFor(4, 4);
    ASSERT_THAT(pass.result()->dispatchWorkgroups(groups[0], groups[1], groups[2]), IsOk());
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
    for (size_t y = 0; y < 4; ++y) {
      std::memcpy(actual->data().data() + y * 16, bytes.result().data() + y * 256,
                  16 * sizeof(float));
    }
    for (size_t pixel = 0; pixel < 16; ++pixel) {
      const size_t offset = pixel * 4;
      std::array<float, 4> straight{};
      const float inputAlpha = inputValues[offset + 3];
      if (inputAlpha > 0) {
        for (size_t channel = 0; channel < 3; ++channel) {
          straight[channel] = inputValues[offset + channel] / inputAlpha;
        }
        straight[3] = inputAlpha;
      }
      std::array<float, 4> transformed{};
      for (size_t channel = 0; channel < transformed.size(); ++channel) {
        transformed[channel] =
            component_transfer::Evaluate(test.functions[channel], straight[channel]);
      }
      for (size_t channel = 0; channel < 3; ++channel) {
        expected->data()[offset + channel] = transformed[channel] * transformed[3];
      }
      expected->data()[offset + 3] = transformed[3];
    }
    ASSERT_THAT(actual->data(),
                testing::Each(testing::Truly([](float value) { return std::isfinite(value); })));
    const auto actualPixels = actual->toPixmap();
    const auto expectedPixels = expected->toPixmap();
    const editor::tests::BitmapGoldenCompareParams compareParams =
        test.allowedMismatchedPixels > 0
            ? editor::tests::ApprovedPixelToleranceParams(0.0f, test.allowedMismatchedPixels,
                                                          /*includeAntiAliasing=*/true)
            : editor::tests::PixelmatchIdentityParams();
    editor::tests::CompareBitmapToBitmap(
        svg::RendererBitmap{
            Vector2i(4, 4),
            std::vector<uint8_t>(actualPixels.data().begin(), actualPixels.data().end()), 16},
        svg::RendererBitmap{
            Vector2i(4, 4),
            std::vector<uint8_t>(expectedPixels.data().begin(), expectedPixels.data().end()), 16},
        "component_transfer_" + std::string(test.name) + "_" +
            std::string(shader.entryPoints.front().name.view()),
        compareParams);
  }
}

}  // namespace donner::gpu::tests
