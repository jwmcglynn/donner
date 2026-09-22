#pragma once
/// @file
/// Native feBlend execution against independent CPU compositing references.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/shader/programs/FilterBlend.h"
#include "donner/gpu/tests/BlendReference.h"
#include "donner/gpu/tests/GpuTestUtils.h"

namespace donner::gpu::tests {
namespace filter_blend_slice {
inline constexpr uint32_t kWidth = 7, kHeight = 5, kRowBytes = 256;
using Pixel = std::array<uint8_t, 4>;
enum class Case { Opaque, Premultiplied, Transparent, Bounds };
struct Scenario {
  uint32_t mode;
  Case kind = Case::Opaque;
};
inline Extent2d InputExtent(Scenario scenario, bool source) {
  if (scenario.kind == Case::Bounds) {
    return source ? Extent2d{3, 2} : Extent2d{5, 3};
  }
  return {kWidth, kHeight};
}
inline Pixel InputPixel(Scenario scenario, uint32_t x, uint32_t y, bool source) {
  if (scenario.kind == Case::Transparent && (x + y) % 2 == uint32_t(source)) {
    return {77, 33, 22, 0};
  }
  if (scenario.kind == Case::Premultiplied) {
    return source ? Pixel{48, 112, 32, 160} : Pixel{128, 32, 64, 208};
  }
  constexpr std::array colors{Pixel{32, 128, 224, 255}, Pixel{224, 32, 128, 255},
                              Pixel{128, 224, 32, 255}, Pixel{224, 128, 32, 255},
                              Pixel{0, 0, 0, 255},      Pixel{255, 255, 255, 255},
                              Pixel{96, 96, 96, 255},   Pixel{64, 128, 64, 255}};
  return colors[(x + y * 3 + (source ? 0 : 2)) % colors.size()];
}
inline std::vector<uint8_t> Expected(Scenario scenario) {
  using tiny_skia::filter::BlendMode;
  using tiny_skia::filter::FloatPixmap;
  auto foreground = FloatPixmap::fromSize(kWidth, kHeight).value();
  auto background = FloatPixmap::fromSize(kWidth, kHeight).value();
  for (bool source : {false, true}) {
    const auto extent = InputExtent(scenario, source);
    auto& image = source ? foreground : background;
    for (uint32_t y = 0; y < extent.height; ++y) {
      for (uint32_t x = 0; x < extent.width; ++x) {
        const auto pixel = InputPixel(scenario, x, y, source);
        for (size_t c = 0; c < 4; ++c) {
          image.data()[(y * kWidth + x) * 4 + c] = pixel[3] == 0 ? 0.0f : pixel[c] / 255.0f;
        }
      }
    }
  }
  if (scenario.mode >= 12 && scenario.mode < 16) {
    return NonseparableBlendReference(scenario.mode, background, foreground);
  }
  constexpr std::array modes{BlendMode::Normal,     BlendMode::Multiply,   BlendMode::Screen,
                             BlendMode::Darken,     BlendMode::Lighten,    BlendMode::Overlay,
                             BlendMode::ColorDodge, BlendMode::ColorBurn,  BlendMode::HardLight,
                             BlendMode::SoftLight,  BlendMode::Difference, BlendMode::Exclusion};
  auto result = FloatPixmap::fromSize(kWidth, kHeight).value();
  tiny_skia::filter::blend(background, foreground, result,
                           scenario.mode < modes.size() ? modes[scenario.mode] : BlendMode::Normal);
  const auto pixels = result.toPixmap();
  return {pixels.data().begin(), pixels.data().end()};
}
inline std::vector<uint8_t> InputBytes(Scenario scenario, bool source) {
  const auto extent = InputExtent(scenario, source);
  std::vector<uint8_t> result(kRowBytes * extent.height);
  for (uint32_t y = 0; y < extent.height; ++y) {
    for (uint32_t x = 0; x < extent.width; ++x) {
      const auto pixel = InputPixel(scenario, x, y, source);
      std::array<float, 4> value{};
      for (size_t c = 0; c < 4; ++c) {
        value[c] = pixel[c] / 255.0f;
      }
      std::memcpy(result.data() + y * kRowBytes + x * sizeof(value), value.data(), sizeof(value));
    }
  }
  return result;
}
}  // namespace filter_blend_slice

/// Dispatches feBlend using reflected roles and a strict independent pixel reference.
/// @param device Native device. @param shader Selected or mutation artifact.
/// @param readbackBuffer Bounded backend readback. @param scenario Blend mode and input case.
template <class DeviceType, class Readback>
void CheckFilterBlend(DeviceType& device, const shader::CompiledShaderView& shader,
                      Readback readbackBuffer, filter_blend_slice::Scenario scenario) {
  using namespace filter_blend_slice;
  ASSERT_THAT(shader.entryPoints, testing::SizeIs(1));
  const auto shape = shader.entryPoints[0].workgroupSize;
  auto module = device.createShaderModule(
      shader::MakeShaderDescriptor(shader, device.shaderSourceKind(), "filter blend"));
  ASSERT_THAT(module, HasResult());
  auto layout = device.createBindGroupLayout({"filter blend", shader::MakeBindingLayout(shader)});
  ASSERT_THAT(layout, HasResult());
  auto pipelineLayout = device.createPipelineLayout({"filter blend", {layout.result()}});
  ASSERT_THAT(pipelineLayout, HasResult());
  auto pipeline =
      device.createComputePipeline({"filter blend",
                                    pipelineLayout.result(),
                                    {module.result(), RcString(shader.entryPoints[0].name.view())},
                                    {shape[0], shape[1], shape[2]}});
  ASSERT_THAT(pipeline, HasResult());
  std::vector<Texture> textures;
  std::vector<TextureView> views;
  std::vector<BindGroupEntry> entries;
  for (bool source : {true, false}) {
    const char* name = source ? "in1_tex" : "in2_tex";
    const auto extent = InputExtent(scenario, source);
    auto texture = device.createTexture(
        {name, extent, TextureFormat::RGBA32Float, TextureUsage::Sampled | TextureUsage::CopyDst});
    ASSERT_THAT(texture, HasResult());
    auto view = device.createTextureView(texture.result(), {name});
    ASSERT_THAT(view, HasResult());
    const auto bytes = InputBytes(scenario, source);
    ASSERT_THAT(device.writeTexture(texture.result(), bytes, {0, kRowBytes, extent.height}, extent),
                IsOk());
    ASSERT_NE(shader.resource(name), nullptr);
    entries.push_back({shader.resource(name)->binding, TextureViewBinding{view.result()}});
    textures.push_back(std::move(texture).result());
    views.push_back(std::move(view).result());
  }
  auto output = device.createTexture({"blend output",
                                      {kWidth, kHeight},
                                      TextureFormat::RGBA32Float,
                                      TextureUsage::StorageBinding | TextureUsage::CopySrc});
  ASSERT_THAT(output, HasResult());
  auto outputView = device.createTextureView(output.result(), {"blend output"});
  ASSERT_THAT(outputView, HasResult());
  ASSERT_NE(shader.resource("output_tex"), nullptr);
  entries.push_back(
      {shader.resource("output_tex")->binding, TextureViewBinding{outputView.result()}});
  const shader::programs::FilterBlendParams params{scenario.mode, 0, 0, 0};
  auto uniform = device.createBuffer(
      {"blend parameters", sizeof(params), BufferUsage::Uniform | BufferUsage::CopyDst});
  ASSERT_THAT(uniform, HasResult());
  ASSERT_THAT(device.writeBuffer(uniform.result(), 0,
                                 {reinterpret_cast<const uint8_t*>(&params), sizeof(params)}),
              IsOk());
  ASSERT_NE(shader.resource("params"), nullptr);
  entries.push_back(
      {shader.resource("params")->binding, BufferBinding{uniform.result(), 0, sizeof(params)}});
  auto group = device.createBindGroup({"filter blend", layout.result(), entries});
  ASSERT_THAT(group, HasResult());
  auto readback = device.createBuffer(
      {"blend readback", kRowBytes * kHeight, BufferUsage::CopyDst | BufferUsage::MapRead});
  ASSERT_THAT(readback, HasResult());
  auto encoder = device.createCommandEncoder();
  ASSERT_THAT(encoder, HasResult());
  auto pass = encoder.result()->beginComputePass({"filter blend"});
  ASSERT_THAT(pass, HasResult());
  ASSERT_THAT(pass.result()->setPipeline(pipeline.result()), IsOk());
  ASSERT_THAT(pass.result()->setBindGroup(0, group.result()), IsOk());
  ASSERT_THAT(pass.result()->dispatchWorkgroups((kWidth + shape[0] - 1) / shape[0],
                                                (kHeight + shape[1] - 1) / shape[1], 1),
              IsOk());
  ASSERT_THAT(pass.result()->end(), IsOk());
  ASSERT_THAT(encoder.result()->copyTextureToBuffer({output.result()}, readback.result(),
                                                    {0, kRowBytes, kHeight}, {kWidth, kHeight}),
              IsOk());
  auto commands = encoder.result()->finish();
  ASSERT_THAT(commands, HasResult());
  auto serial = device.submit(std::move(commands).result());
  ASSERT_THAT(serial, HasResult());
  ASSERT_THAT(device.waitForSerial(serial.result(), 5.0), testing::IsTrue());
  const auto bytes = readbackBuffer(readback.result());
  ASSERT_THAT(bytes, HasResult());
  ASSERT_THAT(bytes.result(), testing::SizeIs(testing::Ge(kRowBytes * kHeight)));
  auto actual = tiny_skia::filter::FloatPixmap::fromSize(kWidth, kHeight).value();
  for (uint32_t y = 0; y < kHeight; ++y) {
    std::memcpy(actual.data().data() + y * kWidth * 4, bytes.result().data() + y * kRowBytes,
                kWidth * 4 * sizeof(float));
  }
  ASSERT_THAT(actual.data(),
              testing::Each(testing::Truly([](float v) { return std::isfinite(v); })));
  const auto pixels = actual.toPixmap();
  editor::tests::CompareBitmapToBitmap(
      svg::RendererBitmap{
          Vector2i(kWidth, kHeight), {pixels.data().begin(), pixels.data().end()}, kWidth * 4},
      svg::RendererBitmap{Vector2i(kWidth, kHeight), Expected(scenario), kWidth * 4},
      "filter_blend_" + std::to_string(scenario.mode) + "_" +
          std::to_string(static_cast<uint32_t>(scenario.kind)),
      editor::tests::PixelmatchIdentityParams());
}
}  // namespace donner::gpu::tests
