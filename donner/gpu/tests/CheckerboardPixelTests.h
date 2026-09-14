#pragma once
/// @file
/// Native checkerboard pixels against the integer-cell and premultiplied blend oracle.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

#include "donner/base/RcString.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/shader/CompiledShader.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/gpu/tests/ReflectedComputeSlice.h"
#include "donner/svg/renderer/geode/GeodeCheckerboardPipeline.h"

namespace donner::gpu::tests {

namespace checkerboard_detail {

/// Draws the exact-byte checkerboard cases through \p pipeline and compares every pixel.
/// @param device Native device with bounded serial wait and buffer readback.
/// @param pipeline Render pipeline to draw with. @param layout Group-zero layout it was built on.
/// @param uniformBinding Slot of the uniform block within \p layout.
/// @param destinationOver Whether \p pipeline composites under existing premultiplied content.
/// @param labelPrefix Comparator label prefix, distinct per pipeline source.
template <typename NativeDevice>
void RunCheckerboardCases(NativeDevice& device, const RenderPipeline& pipeline,
                          const BindGroupLayout& layout, uint32_t uniformBinding,
                          bool destinationOver, const std::string& labelPrefix) {
  constexpr uint32_t width = 64;
  constexpr uint32_t height = 48;
  constexpr uint32_t pitch = width * 4;
  using Pipeline = geode::GeodeCheckerboardPipeline;
  const std::array<std::array<float, 2>, 4> origins{
      {{0, 0}, {-33, -17}, {9, 21}, {-0.75f, 16.25f}}};
  const std::array<std::array<uint8_t, 4>, 3> backgrounds{
      {{0, 0, 0, 0}, {16, 20, 24, 128}, {48, 52, 56, 255}}};
  for (float dpr : {1.f, 2.f}) {
    for (const auto& origin : origins) {
      for (bool scissor : {false, true}) {
        for (const auto& background : backgrounds) {
          SCOPED_TRACE(testing::Message()
                       << "under=" << destinationOver << " dpr=" << dpr << " origin=" << origin[0]
                       << ',' << origin[1] << " scissor=" << scissor
                       << " alpha=" << int(background[3]));
          Pipeline::Uniforms uniforms{{float(width), float(height)},
                                      1,
                                      16,
                                      {40.f / 255, 40.f / 255, 40.f / 255, 1},
                                      {60.f / 255, 60.f / 255, 60.f / 255, 1},
                                      {},
                                      {}};
          uniforms.devicePixelRatio = dpr;
          uniforms.originOffsetPx[0] = origin[0];
          uniforms.originOffsetPx[1] = origin[1];
          auto uniform = device.createBuffer(BufferDescriptor{
              "params", sizeof(uniforms), BufferUsage::Uniform | BufferUsage::CopyDst});
          ASSERT_THAT(uniform, HasResult());
          ASSERT_THAT(device.writeBuffer(
                          uniform.result(), 0,
                          std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&uniforms),
                                                   sizeof(uniforms))),
                      IsOk());
          auto group = device.createBindGroup(BindGroupDescriptor{
              "params",
              layout,
              {{uniformBinding, BufferBinding{uniform.result(), 0, sizeof(uniforms)}}}});
          ASSERT_THAT(group, HasResult());
          auto target = device.createTexture(
              TextureDescriptor{"target",
                                {width, height},
                                TextureFormat::RGBA8Unorm,
                                TextureUsage::RenderAttachment | TextureUsage::CopySrc});
          ASSERT_THAT(target, HasResult());
          auto view = device.createTextureView(target.result(), TextureViewDescriptor{"target"});
          ASSERT_THAT(view, HasResult());
          auto readback = device.createBuffer(BufferDescriptor{
              "pixels", pitch * height, BufferUsage::CopyDst | BufferUsage::MapRead});
          ASSERT_THAT(readback, HasResult());
          auto encoder = device.createCommandEncoder();
          ASSERT_THAT(encoder, HasResult());
          std::array<double, 4> clear{};
          for (size_t channel = 0; channel < 4; ++channel)
            clear[channel] = background[channel] / 255.0;
          auto pass = encoder.result()->beginRenderPass(RenderPassDescriptor{
              "checkerboard", {{view.result(), LoadOp::Clear, StoreOp::Store, clear}}});
          ASSERT_THAT(pass, HasResult());
          ASSERT_THAT(pass.result()->setPipeline(pipeline), IsOk());
          ASSERT_THAT(pass.result()->setBindGroup(0, group.result()), IsOk());
          if (scissor) ASSERT_THAT(pass.result()->setScissorRect(7, 5, 41, 29), IsOk());
          ASSERT_THAT(pass.result()->draw(3), IsOk());
          ASSERT_THAT(pass.result()->end(), IsOk());
          ASSERT_THAT(encoder.result()->copyTextureToBuffer({target.result()}, readback.result(),
                                                            {0, pitch, height}, {width, height}),
                      IsOk());
          auto commands = encoder.result()->finish();
          ASSERT_THAT(commands, HasResult());
          auto serial = device.submit(std::move(commands).result());
          ASSERT_THAT(serial, HasResult());
          ASSERT_TRUE(device.waitForSerial(serial.result(), 30.0));
          ASSERT_THAT(device.lastErrorForTest(), testing::IsEmpty());
          auto pixels = device.readBackBuffer(readback.result());
          ASSERT_THAT(pixels, HasResult());
          ASSERT_EQ(pixels.result().size(), pitch * height);
          svg::RendererBitmap expectedBitmap;
          expectedBitmap.dimensions = Vector2i(width, height);
          expectedBitmap.rowBytes = pitch;
          expectedBitmap.alphaType = svg::AlphaType::Premultiplied;
          expectedBitmap.pixels.resize(pitch * height);
          for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x) {
              std::array<uint8_t, 4> expected = background;
              if (!scissor || (x >= 7 && x < 48 && y >= 5 && y < 34)) {
                const int32_t cx =
                    static_cast<int32_t>(std::floor(((x + 0.5f + origin[0]) / dpr) / 16.f));
                const int32_t cy =
                    static_cast<int32_t>(std::floor(((y + 0.5f + origin[1]) / dpr) / 16.f));
                const bool light = ((cx + cy) & 1) == 0;
                expected = light ? std::array<uint8_t, 4>{60, 60, 60, 255}
                                 : std::array<uint8_t, 4>{40, 40, 40, 255};
                if (destinationOver && background[3] == 128) {
                  expected = light ? std::array<uint8_t, 4>{46, 50, 54, 255}
                                   : std::array<uint8_t, 4>{36, 40, 44, 255};
                } else if (destinationOver && background[3] == 255) {
                  expected = background;
                }
                expected[3] = 255;
              }
              std::copy(expected.begin(), expected.end(),
                        expectedBitmap.pixels.begin() + y * pitch + x * 4);
            }
          svg::RendererBitmap actual = expectedBitmap;
          actual.pixels = std::move(pixels).result();
          editor::tests::CompareBitmapToBitmap(
              actual, expectedBitmap,
              labelPrefix + std::to_string(destinationOver) + "_" + std::to_string(dpr) + "_" +
                  std::to_string(origin[0]) + "_" + std::to_string(origin[1]) + "_" +
                  std::to_string(scissor) + "_" + std::to_string(background[3]),
              editor::tests::PixelmatchIdentityParams());
        }
      }
    }
  }
}

}  // namespace checkerboard_detail

/// Runs the same exact-byte checkerboard cases on native Metal and Vulkan through the production
/// pipeline in both blend modes.
/// @param device Native device with bounded serial wait and buffer readback.
template <typename NativeDevice>
void ExpectCheckerboardPixels(NativeDevice& device) {
  using Pipeline = geode::GeodeCheckerboardPipeline;
  for (bool destinationOver : {false, true}) {
    Pipeline pipeline(
        device, TextureFormat::RGBA8Unorm,
        destinationOver ? Pipeline::BlendMode::DestinationOver : Pipeline::BlendMode::Replace);
    ASSERT_TRUE(pipeline.valid()) << "The selected native checkerboard pipeline must compile";
    checkerboard_detail::RunCheckerboardCases(device, pipeline.pipeline(),
                                              pipeline.bindGroupLayout(), pipeline.uniformBinding(),
                                              destinationOver, "checkerboard_");
    if (testing::Test::HasFatalFailure()) return;
  }
}

/// Builds a replace-mode render pipeline straight from \p shader, using its reflected entry names
/// and uniform slot, and runs the checkerboard oracle against it. This is how a mutation control
/// with renamed entries and a moved binding proves the pipeline reads the interface it was given.
/// @param device Native device with bounded serial wait and buffer readback.
/// @param shader Selected or mutation checkerboard artifact.
template <typename NativeDevice>
void ExpectCheckerboardShaderPixels(NativeDevice& device,
                                    const shader::CompiledShaderView& shader) {
  ASSERT_THAT(shader.entryPoints, testing::SizeIs(2));
  ASSERT_EQ(shader.entryPoints[0].stage, ShaderStage::Vertex);
  ASSERT_EQ(shader.entryPoints[1].stage, ShaderStage::Fragment);
  auto module = device.createShaderModule(
      shader::MakeShaderDescriptor(shader, device.shaderSourceKind(), "checkerboard"));
  ASSERT_THAT(module, HasResult());
  auto layout = device.createBindGroupLayout(
      BindGroupLayoutDescriptor{"checkerboard", shader::MakeBindingLayout(shader)});
  ASSERT_THAT(layout, HasResult());
  auto pipelineLayout =
      device.createPipelineLayout(PipelineLayoutDescriptor{"checkerboard", {layout.result()}});
  ASSERT_THAT(pipelineLayout, HasResult());
  auto pipeline = device.createRenderPipeline(RenderPipelineDescriptor{
      "checkerboard", pipelineLayout.result(),
      VertexState{module.result(), RcString(shader.entryPoints[0].name.view()), {}},
      FragmentState{module.result(),
                    RcString(shader.entryPoints[1].name.view()),
                    {ColorTargetState{TextureFormat::RGBA8Unorm}}},
      PrimitiveTopology::TriangleList, CullMode::None});
  ASSERT_THAT(pipeline, HasResult());
  checkerboard_detail::RunCheckerboardCases(
      device, pipeline.result(), layout.result(), ReflectedBinding(shader, "params"), false,
      "checkerboard_" + std::string(shader.entryPoints[1].name.view()) + "_");
}

}  // namespace donner::gpu::tests
