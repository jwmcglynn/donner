#pragma once
/// @file
/// Native execution of the authored Slug mask against an independent rectangle oracle.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/shader/programs/SlugMask.h"
#include "donner/gpu/tests/GpuTestUtils.h"

namespace donner::gpu::tests {
namespace slug_mask_slice {

inline constexpr uint32_t kWidth = 8, kHeight = 8, kRowBytes = 256;
inline constexpr float kLeft = 1.25f, kTop = 1.0f, kRight = 6.75f, kBottom = 6.0f;

/// Independent cases exercise mask coverage, nested clipping and buffer-range robustness.
enum class Case { Analytic, Binary, NestedClip, DoubleNonzero, DoubleEvenOdd, DeclaredRange };

inline bool Doubled(Case testCase) {
  return testCase == Case::DoubleNonzero || testCase == Case::DoubleEvenOdd;
}

inline shader::programs::SlugMaskParams Parameters(Case testCase) {
  shader::programs::SlugMaskParams result{};
  result.mvp[0] = 2.0f / kWidth;
  result.mvp[5] = -2.0f / kHeight;
  result.mvp[10] = result.mvp[15] = 1.0f;
  result.mvp[12] = -1.0f;
  result.mvp[13] = 1.0f;
  // Path space is target pixels, so a pixel center maps to itself.
  result.pathFromPixel[0] = result.pathFromPixel[3] = 1.0f;
  result.viewport[0] = kWidth;
  result.viewport[1] = kHeight;
  result.antialias = testCase == Case::Analytic || testCase == Case::NestedClip;
  result.hasClipMask = testCase == Case::NestedClip;
  result.fillRule = testCase == Case::DoubleEvenOdd ? 1u : 0u;
  result.gridHBandCount = result.gridVBandCount = 1;
  result.gridHStride = result.gridVStride = 8.0f;
  result.boundingVertexCount = 4;
  const std::array<float, 8> vertices{kLeft, kTop, kRight, kTop, kRight, kBottom, kLeft, kBottom};
  std::copy(vertices.begin(), vertices.end(), result.boundingVertices);
  return result;
}

inline uint8_t ClipValue(uint32_t x, uint32_t y) {
  constexpr std::array<uint8_t, 4> values{64, 128, 192, 255};
  return values[(x + 2 * y) % values.size()];
}

inline std::vector<uint8_t> Expected(Case testCase) {
  std::vector<uint8_t> bytes(kWidth * kHeight * 4);
  for (uint32_t y = 0; y < kHeight; ++y) {
    for (uint32_t x = 0; x < kWidth; ++x) {
      float coverage = 0.0f;
      if (y + 0.5f >= kTop && y + 0.5f < kBottom) {
        coverage = testCase == Case::Analytic || testCase == Case::NestedClip
                       ? std::max(0.0f, std::min(float(x + 1), kRight) - std::max(float(x), kLeft))
                       : float(x + 0.5f >= kLeft && x + 0.5f < kRight);
      }
      if (testCase == Case::DoubleEvenOdd || testCase == Case::DeclaredRange) {
        coverage = 0.0f;
      }
      const uint32_t scale = testCase == Case::NestedClip ? ClipValue(x, y) : 255;
      const uint8_t value = static_cast<uint8_t>(std::lround(coverage * scale));
      std::fill_n(bytes.begin() + (y * kWidth + x) * 4, 4, value);
    }
  }
  return bytes;
}

inline bool UploadBinding(Device& device, const shader::CompiledShaderView& shader,
                          const char* name, std::span<const uint8_t> bytes, uint64_t range,
                          std::vector<Buffer>& buffers, std::vector<BindGroupEntry>& entries) {
  const auto* resource = shader.resource(name);
  EXPECT_NE(resource, nullptr);
  if (!resource) {
    return false;
  }
  const BufferUsage usage =
      resource->type == BindingType::UniformBuffer ? BufferUsage::Uniform : BufferUsage::Storage;
  auto buffer = device.createBuffer({name, bytes.size(), usage | BufferUsage::CopyDst});
  EXPECT_THAT(buffer, HasResult());
  if (buffer.hasError()) {
    return false;
  }
  const auto write = device.writeBuffer(buffer.result(), 0, bytes);
  EXPECT_THAT(write, IsOk());
  if (write.hasError()) {
    return false;
  }
  buffers.push_back(std::move(buffer).result());
  entries.push_back({resource->binding, BufferBinding{buffers.back(), 0, range}});
  return true;
}

/// Uploads two line segments per axis, sorted by descending maximum ray coordinate.
/// Tail segments are physically present but outside the declared range, so an incorrect
/// allocation-length query would turn the DeclaredRange case into an opaque rectangle.
inline bool UploadGeometry(Device& device, const shader::CompiledShaderView& shader, Case testCase,
                           std::vector<Buffer>& buffers, std::vector<BindGroupEntry>& entries) {
  const shader::programs::SlugMaskBand band{0, Doubled(testCase) ? 4u : 2u};
  const std::array<float, 12> horizontal{
      kRight, kTop,    kRight, (kTop + kBottom) * 0.5f, kRight, kBottom,
      kLeft,  kBottom, kLeft,  (kTop + kBottom) * 0.5f, kLeft,  kTop};
  const std::array<float, 12> vertical{
      kRight, kBottom, (kLeft + kRight) * 0.5f, kBottom, kLeft,  kBottom,
      kLeft,  kTop,    (kLeft + kRight) * 0.5f, kTop,    kRight, kTop};
  std::array<float, 24> hData{}, vData{};
  std::copy(horizontal.begin(), horizontal.end(), hData.begin());
  std::copy(horizontal.begin(), horizontal.end(), hData.begin() + 12);
  std::copy(vertical.begin(), vertical.end(), vData.begin());
  std::copy(vertical.begin(), vertical.end(), vData.begin() + 12);
  const std::array<uint32_t, 4> refs = testCase == Case::DeclaredRange
                                           ? std::array<uint32_t, 4>{2, 3, 0, 0}
                                       : Doubled(testCase) ? std::array<uint32_t, 4>{0, 0, 1, 1}
                                                           : std::array<uint32_t, 4>{0, 1, 0, 0};
  const uint32_t grid = 0;
  const auto upload = [&](const char* name, const auto& value, uint64_t range) {
    return UploadBinding(device, shader, name,
                         {reinterpret_cast<const uint8_t*>(&value), sizeof(value)}, range, buffers,
                         entries);
  };
  return upload("bands", band, sizeof(band)) && upload("vBands", band, sizeof(band)) &&
         upload("curveData", hData, sizeof(horizontal)) &&
         upload("vCurveData", vData, sizeof(vertical)) && upload("hBandGrid", grid, sizeof(grid)) &&
         upload("vBandGrid", grid, sizeof(grid)) &&
         upload("hCurveIndices", refs, band.curveCount * sizeof(uint32_t)) &&
         upload("vCurveIndices", refs, band.curveCount * sizeof(uint32_t));
}

}  // namespace slug_mask_slice

/// Renders the real mask program and checks every channel using the repository bitmap comparator.
/// @param device Native backend under test. @param shader Frozen selected or test projections.
/// @param readbackBuffer Backend's bounded buffer readback. @param testCase Reference case.
template <typename DeviceType, typename Readback>
void CheckSlugMask(DeviceType& device, const shader::CompiledShaderView& shader,
                   Readback readbackBuffer, slug_mask_slice::Case testCase) {
  using namespace slug_mask_slice;
  ASSERT_THAT(shader.entryPoints, testing::SizeIs(2));
  auto module = device.createShaderModule(
      shader::MakeShaderDescriptor(shader, device.shaderSourceKind(), "Slug mask"));
  ASSERT_THAT(module, HasResult());
  auto layout = device.createBindGroupLayout({"Slug mask", shader::MakeBindingLayout(shader)});
  ASSERT_THAT(layout, HasResult());
  auto pipelineLayout = device.createPipelineLayout({"Slug mask", {layout.result()}});
  ASSERT_THAT(pipelineLayout, HasResult());
  const BlendState maxBlend{{BlendFactor::One, BlendFactor::One, BlendOperation::Max},
                            {BlendFactor::One, BlendFactor::One, BlendOperation::Max}};
  auto pipeline = device.createRenderPipeline(
      {"Slug mask",
       pipelineLayout.result(),
       {module.result(), RcString(shader.entryPoints[0].name.view()), {}},
       FragmentState{module.result(),
                     RcString(shader.entryPoints[1].name.view()),
                     {{TextureFormat::RGBA8Unorm, maxBlend}}},
       PrimitiveTopology::TriangleList,
       CullMode::None});
  ASSERT_THAT(pipeline, HasResult());
  auto output = device.createTexture({"mask output",
                                      {kWidth, kHeight},
                                      TextureFormat::RGBA8Unorm,
                                      TextureUsage::RenderAttachment | TextureUsage::CopySrc});
  auto clip = device.createTexture({"nested clip",
                                    {kWidth, kHeight},
                                    TextureFormat::RGBA8Unorm,
                                    TextureUsage::Sampled | TextureUsage::CopyDst});
  ASSERT_THAT(output, HasResult());
  ASSERT_THAT(clip, HasResult());
  auto outputView = device.createTextureView(output.result(), {"mask output"});
  auto clipView = device.createTextureView(clip.result(), {"nested clip"});
  ASSERT_THAT(outputView, HasResult());
  ASSERT_THAT(clipView, HasResult());
  std::array<uint8_t, kRowBytes * kHeight> clipBytes{};
  for (uint32_t y = 0; y < kHeight; ++y) {
    for (uint32_t x = 0; x < kWidth; ++x) {
      std::fill_n(clipBytes.begin() + y * kRowBytes + x * 4, 4, ClipValue(x, y));
    }
  }
  ASSERT_THAT(
      device.writeTexture(clip.result(), clipBytes, {0, kRowBytes, kHeight}, {kWidth, kHeight}),
      IsOk());
  std::vector<Buffer> buffers;
  std::vector<BindGroupEntry> entries;
  const auto params = Parameters(testCase);
  ASSERT_EQ(UploadBinding(device, shader, "uniforms",
                          {reinterpret_cast<const uint8_t*>(&params), sizeof(params)},
                          sizeof(params), buffers, entries),
            true);
  ASSERT_EQ(UploadGeometry(device, shader, testCase, buffers, entries), true);
  ASSERT_NE(shader.resource("clipMaskTexture"), nullptr);
  entries.push_back(
      {shader.resource("clipMaskTexture")->binding, TextureViewBinding{clipView.result()}});
  auto bindGroup = device.createBindGroup({"Slug mask", layout.result(), entries});
  ASSERT_THAT(bindGroup, HasResult());
  auto readback = device.createBuffer(
      {"mask readback", kRowBytes * kHeight, BufferUsage::CopyDst | BufferUsage::MapRead});
  ASSERT_THAT(readback, HasResult());
  auto encoder = device.createCommandEncoder();
  ASSERT_THAT(encoder, HasResult());
  auto pass = encoder.result()->beginRenderPass(
      {"Slug mask", {{outputView.result(), LoadOp::Clear, StoreOp::Store, {0, 0, 0, 0}}}});
  ASSERT_THAT(pass, HasResult());
  ASSERT_THAT(pass.result()->setPipeline(pipeline.result()), IsOk());
  ASSERT_THAT(pass.result()->setBindGroup(0, bindGroup.result()), IsOk());
  ASSERT_THAT(pass.result()->draw(6, 1, 0, 0), IsOk());
  ASSERT_THAT(pass.result()->end(), IsOk());
  ASSERT_THAT(encoder.result()->copyTextureToBuffer({output.result()}, readback.result(),
                                                    {0, kRowBytes, kHeight}, {kWidth, kHeight}),
              IsOk());
  auto commands = encoder.result()->finish();
  ASSERT_THAT(commands, HasResult());
  auto serial = device.submit(std::move(commands).result());
  ASSERT_THAT(serial, HasResult());
  ASSERT_THAT(device.waitForSerial(serial.result(), 5.0), testing::IsTrue());
  auto bytes = readbackBuffer(readback.result());
  ASSERT_THAT(bytes, HasResult());
  ASSERT_THAT(bytes.result(), testing::SizeIs(testing::Ge(kRowBytes * kHeight)));
  std::vector<uint8_t> pixels(kWidth * kHeight * 4);
  for (uint32_t y = 0; y < kHeight; ++y) {
    std::memcpy(pixels.data() + y * kWidth * 4, bytes.result().data() + y * kRowBytes, kWidth * 4);
  }
  editor::tests::CompareBitmapToBitmap(
      svg::RendererBitmap{Vector2i(kWidth, kHeight), pixels, kWidth * 4},
      svg::RendererBitmap{Vector2i(kWidth, kHeight), Expected(testCase), kWidth * 4},
      "slug_mask_" + std::to_string(static_cast<unsigned>(testCase)),
      editor::tests::PixelmatchIdentityParams());
}

}  // namespace donner::gpu::tests
