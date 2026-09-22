#pragma once
/// @file
/// Native Slug fill rendering against independent rectangle and paint references.

#include "donner/gpu/shader/programs/SlugFill.h"
#include "donner/gpu/tests/SlugMaskSlice.h"
#include "tiny_skia/filter/Blend.h"

namespace donner::gpu::tests {
namespace slug_fill_slice {
using Pixel = std::array<uint8_t, 4>;
inline constexpr uint32_t kWidth = 8, kHeight = 8, kRowBytes = 256;
inline constexpr Pixel kUniformColor{128, 64, 32, 128}, kRecordColor{32, 96, 64, 128};
inline constexpr Pixel kPattern{64, 128, 192, 255};
enum class Case {
  Analytic,
  Binary,
  EvenOdd,
  Clip,
  ClipRect,
  Pattern,
  Batched,
  FirstInstance,
  Overlap,
  DeclaredRange,
  LinearGradient,
  RadialGradient,
  AnalyticEvenOdd,
  BatchedPattern,
  BatchedClipRect
};
inline bool Batched(Case c) {
  return c >= Case::Batched && c != Case::AnalyticEvenOdd;
}
inline uint32_t First(Case c) {
  return c == Case::FirstInstance || c == Case::DeclaredRange ? 1u : 0u;
}
inline uint32_t Count(Case c) {
  return c == Case::Overlap ? 2u : 1u;
}
inline uint8_t ClipValue(uint32_t x, uint32_t y) {
  constexpr std::array<uint8_t, 4> values{0, 85, 170, 255};
  return values[(x + y) % 4];
}
template <class T>
void Geometry(T& p) {
  p.gridHBandCount = p.gridVBandCount = 1;
  p.gridHStride = p.gridVStride = 8;
  p.boundingVertexCount = 4;
  constexpr std::array<float, 8> vertices{1.25f, 1, 6.75f, 1, 6.75f, 6, 1.25f, 6};
  std::copy(vertices.begin(), vertices.end(), p.boundingVertices);
  p.vGridBase = 1;
  p.hRefsBase = 2;
  p.vRefsBase = 4;
  p.patternOpacity = 0.75f;
}
inline shader::programs::SlugFillParams Parameters(Case c) {
  shader::programs::SlugFillParams p{};
  Geometry(p);
  p.mvp[0] = 2.0f / kWidth;
  p.mvp[5] = -2.0f / kHeight;
  p.mvp[10] = p.mvp[15] = 1;
  p.mvp[12] = -1;
  p.mvp[13] = 1;
  p.patternFromPath[0] = p.patternFromPath[5] = p.patternFromPath[10] = p.patternFromPath[15] = 1;
  p.viewport[0] = kWidth;
  p.viewport[1] = kHeight;
  p.tileSize[0] = p.tileSize[1] = 1;
  p.antialias = c != Case::Binary && c != Case::EvenOdd;
  p.hasClipMask = c == Case::Clip;
  p.clipRectActive = c == Case::ClipRect;
  p.clipRect[0] = 2;
  p.clipRect[1] = 2;
  p.clipRect[2] = 6;
  p.clipRect[3] = 5;
  p.fillRule = c == Case::EvenOdd || c == Case::AnalyticEvenOdd;
  p.paintMode = c == Case::Pattern ? 1u : 0u;
  for (size_t i = 0; i < 4; ++i) {
    p.color[i] = kUniformColor[i] / 255.0f;
  }
  return p;
}
inline std::array<shader::programs::SlugFillInstance, 2> Records(Case c) {
  std::array<shader::programs::SlugFillInstance, 2> result{};
  for (size_t n = 0; n < result.size(); ++n) {
    auto& p = result[n];
    Geometry(p);
    p.transformRow0[0] = p.transformRow1[1] = 1;
    const Pixel color = n == 0 ? kRecordColor : kUniformColor;
    for (size_t i = 0; i < 4; ++i) {
      p.color[i] = color[i] / 255.0f;
    }
    p.gradientStopCount = 2;
    if (n == 1 && (c == Case::FirstInstance || c == Case::DeclaredRange)) {
      p.transformRow0[2] = 1;
    }
    if (c == Case::BatchedPattern) {
      p.paintMode = 1;
    }
    if (c == Case::BatchedClipRect) {
      p.clipRectActive = 1;
      p.clipRect[0] = 2;
      p.clipRect[1] = 2;
      p.clipRect[2] = 6;
      p.clipRect[3] = 5;
    }
    if (c == Case::LinearGradient) {
      p.paintMode = 2;
    }
    if (c == Case::RadialGradient) {
      p.paintMode = 3;
    }
  }
  return result;
}
inline bool OutsideClip(Case c, float px, float py) {
  if (c != Case::ClipRect && c != Case::BatchedClipRect) {
    return false;
  }
  return px < 2 || px >= 6 || py < 2 || py >= 5;
}
inline float Coverage(Case c, uint32_t x, uint32_t y) {
  const float shift = c == Case::FirstInstance ? 1.0f : 0.0f;
  const float px = x + 0.5f - shift, py = y + 0.5f;
  if (py < 1 || py >= 6 || c == Case::EvenOdd) {
    return 0;
  }
  if (OutsideClip(c, px, py)) {
    return 0;
  }
  float coverage = c == Case::Binary ? float(px >= 1.25f && px < 6.75f)
                                     : std::max(0.0f, std::min(float(x + 1) - shift, 6.75f) -
                                                          std::max(float(x) - shift, 1.25f));
  if (c == Case::Clip) {
    coverage *= ClipValue(x, y) / 255.0f;
  }
  return coverage;
}
inline std::array<float, 4> Color(Case c, uint32_t x, uint32_t y, uint32_t instance) {
  if (c == Case::LinearGradient || c == Case::RadialGradient) {
    const float t = c == Case::LinearGradient ? (x + 0.5f) / 8
                                              : std::min(std::hypot(x + 0.5f, y + 0.5f) / 8, 1.0f);
    return {(32 + 64 * t) / 255, (64 + 64 * t) / 255, (128 + 64 * t) / 255, 1};
  }
  const Pixel bytes = (c == Case::Pattern || c == Case::BatchedPattern) ? kPattern
                      : !Batched(c)                                     ? kUniformColor
                      : instance == 0                                   ? kRecordColor
                                                                        : kUniformColor;
  std::array<float, 4> result{};
  for (size_t i = 0; i < 4; ++i) {
    result[i] =
        bytes[i] / 255.0f * ((c == Case::Pattern || c == Case::BatchedPattern) ? 0.75f : 1.0f);
  }
  return result;
}
inline std::vector<uint8_t> Expected(Case c) {
  using tiny_skia::filter::FloatPixmap;
  auto background = FloatPixmap::fromSize(kWidth, kHeight).value();
  for (uint32_t instance = First(c); instance < First(c) + Count(c); ++instance) {
    auto foreground = FloatPixmap::fromSize(kWidth, kHeight).value();
    auto output = FloatPixmap::fromSize(kWidth, kHeight).value();
    for (uint32_t y = 0; y < kHeight; ++y) {
      for (uint32_t x = 0; x < kWidth; ++x) {
        const auto color = Color(c, x, y, c == Case::DeclaredRange ? 0 : instance);
        const float coverage = Coverage(c, x, y);
        for (size_t lane = 0; lane < 4; ++lane) {
          foreground.data()[(y * kWidth + x) * 4 + lane] = color[lane] * coverage;
        }
      }
    }
    tiny_skia::filter::blend(background, foreground, output, tiny_skia::filter::BlendMode::Normal);
    background = FloatPixmap::fromPixmap(output.toPixmap());
  }
  const auto pixels = background.toPixmap();
  return {pixels.data().begin(), pixels.data().end()};
}
}  // namespace slug_fill_slice

/// Renders the real four-entry Slug program with strict independent pixel acceptance.
/// @param device Native device. @param shader Frozen selected or test projections.
/// @param readbackBuffer Bounded backend readback. @param testCase Reference scenario.
template <class DeviceType, class Readback>
void CheckSlugFill(DeviceType& device, const shader::CompiledShaderView& shader,
                   Readback readbackBuffer, slug_fill_slice::Case testCase) {
  using namespace slug_fill_slice;
  ASSERT_THAT(shader.entryPoints, testing::SizeIs(4));
  auto module = device.createShaderModule(
      shader::MakeShaderDescriptor(shader, device.shaderSourceKind(), "Slug fill"));
  ASSERT_THAT(module, HasResult());
  auto layout = device.createBindGroupLayout({"Slug fill", shader::MakeBindingLayout(shader)});
  ASSERT_THAT(layout, HasResult());
  auto pipelineLayout = device.createPipelineLayout({"Slug fill", {layout.result()}});
  ASSERT_THAT(pipelineLayout, HasResult());
  const BlendState over{{BlendFactor::One, BlendFactor::OneMinusSrcAlpha, BlendOperation::Add},
                        {BlendFactor::One, BlendFactor::OneMinusSrcAlpha, BlendOperation::Add}};
  auto pipeline = device.createRenderPipeline(
      {"Slug fill",
       pipelineLayout.result(),
       {module.result(), RcString(shader.entryPoints[Batched(testCase) ? 1 : 0].name.view()), {}},
       FragmentState{module.result(),
                     RcString(shader.entryPoints[Batched(testCase) ? 3 : 2].name.view()),
                     {{TextureFormat::RGBA8Unorm, over}}},
       PrimitiveTopology::TriangleList,
       CullMode::None});
  ASSERT_THAT(pipeline, HasResult());
  auto output = device.createTexture({"fill output",
                                      {kWidth, kHeight},
                                      TextureFormat::RGBA8Unorm,
                                      TextureUsage::RenderAttachment | TextureUsage::CopySrc});
  ASSERT_THAT(output, HasResult());
  auto outputView = device.createTextureView(output.result(), {"fill output"});
  ASSERT_THAT(outputView, HasResult());
  auto clip = device.createTexture({"clip",
                                    {kWidth, kHeight},
                                    TextureFormat::RGBA8Unorm,
                                    TextureUsage::Sampled | TextureUsage::CopyDst});
  ASSERT_THAT(clip, HasResult());
  auto clipView = device.createTextureView(clip.result(), {"clip"});
  ASSERT_THAT(clipView, HasResult());
  auto pattern = device.createTexture({"pattern",
                                       {1, 1},
                                       TextureFormat::RGBA8Unorm,
                                       TextureUsage::Sampled | TextureUsage::CopyDst});
  ASSERT_THAT(pattern, HasResult());
  auto patternView = device.createTextureView(pattern.result(), {"pattern"});
  ASSERT_THAT(patternView, HasResult());
  auto sampler = device.createSampler({"pattern"});
  ASSERT_THAT(sampler, HasResult());
  std::array<uint8_t, kRowBytes * kHeight> clipBytes{};
  for (uint32_t y = 0; y < kHeight; ++y) {
    for (uint32_t x = 0; x < kWidth; ++x) {
      std::fill_n(clipBytes.begin() + y * kRowBytes + x * 4, 4, ClipValue(x, y));
    }
  }
  ASSERT_THAT(
      device.writeTexture(clip.result(), clipBytes, {0, kRowBytes, kHeight}, {kWidth, kHeight}),
      IsOk());
  std::array<uint8_t, kRowBytes> patternBytes{};
  std::copy(kPattern.begin(), kPattern.end(), patternBytes.begin());
  ASSERT_THAT(device.writeTexture(pattern.result(), patternBytes, {0, kRowBytes, 1}, {1, 1}),
              IsOk());
  const auto params = Parameters(testCase);
  const auto records = Records(testCase);
  const shader::programs::SlugFillBand band{0, testCase == Case::EvenOdd ? 4u : 2u};
  const std::array<float, 12> h{6.75f, 1, 6.75f, 3.5f, 6.75f, 6, 1.25f, 6, 1.25f, 3.5f, 1.25f, 1};
  const std::array<float, 12> v{6.75f, 6, 4, 6, 1.25f, 6, 1.25f, 1, 4, 1, 6.75f, 1};
  const std::array<uint32_t, 6> grid{0, 0, 0, 1, 0, 1};
  const std::array<uint32_t, 10> doubledGrid{0, 0, 0, 0, 1, 1, 0, 0, 1, 1};
  auto actualParams = params;
  if (testCase == Case::EvenOdd) {
    actualParams.vRefsBase = 6;
  }
  std::array<float, 100> paint{};
  paint[0] = paint[5] = 1;
  paint[10] = 8;
  paint[16] = 8;
  const std::array<float, 4> color0{32 / 255.0f, 64 / 255.0f, 128 / 255.0f, 1},
      color1{96 / 255.0f, 128 / 255.0f, 192 / 255.0f, 1};
  std::copy(color0.begin(), color0.end(), paint.begin() + 20);
  std::copy(color1.begin(), color1.end(), paint.begin() + 24);
  paint[85] = 1;
  std::vector<Buffer> buffers;
  std::vector<BindGroupEntry> entries;
  const auto upload = [&](const char* name, const auto& data, uint64_t range) {
    return slug_mask_slice::UploadBinding(device, shader, name,
                                          {reinterpret_cast<const uint8_t*>(&data), sizeof(data)},
                                          range, buffers, entries);
  };
  ASSERT_EQ(upload("uniforms", actualParams, sizeof(actualParams)), true);
  ASSERT_EQ(upload("instances", records,
                   testCase == Case::DeclaredRange ? sizeof(records[0]) : sizeof(records)),
            true);
  ASSERT_EQ(upload("bands", band, sizeof(band)), true);
  ASSERT_EQ(upload("vBands", band, sizeof(band)), true);
  ASSERT_EQ(upload("curveData", h, sizeof(h)), true);
  ASSERT_EQ(upload("vCurveData", v, sizeof(v)), true);
  if (testCase == Case::EvenOdd) {
    ASSERT_EQ(upload("gridData", doubledGrid, sizeof(doubledGrid)), true);
  } else {
    ASSERT_EQ(upload("gridData", grid, sizeof(grid)), true);
  }
  ASSERT_EQ(upload("paintData", paint, sizeof(paint)), true);
  for (const char* name : {"clipMaskTexture", "patternTexture", "patternSampler"}) {
    ASSERT_NE(shader.resource(name), nullptr);
  }
  entries.push_back(
      {shader.resource("clipMaskTexture")->binding, TextureViewBinding{clipView.result()}});
  entries.push_back(
      {shader.resource("patternTexture")->binding, TextureViewBinding{patternView.result()}});
  entries.push_back({shader.resource("patternSampler")->binding, SamplerBinding{sampler.result()}});
  auto group = device.createBindGroup({"Slug fill", layout.result(), entries});
  ASSERT_THAT(group, HasResult());
  auto readback = device.createBuffer(
      {"fill readback", kRowBytes * kHeight, BufferUsage::CopyDst | BufferUsage::MapRead});
  ASSERT_THAT(readback, HasResult());
  auto encoder = device.createCommandEncoder();
  ASSERT_THAT(encoder, HasResult());
  auto pass = encoder.result()->beginRenderPass(
      {"Slug fill", {{outputView.result(), LoadOp::Clear, StoreOp::Store, {0, 0, 0, 0}}}});
  ASSERT_THAT(pass, HasResult());
  ASSERT_THAT(pass.result()->setPipeline(pipeline.result()), IsOk());
  ASSERT_THAT(pass.result()->setBindGroup(0, group.result()), IsOk());
  ASSERT_THAT(pass.result()->draw(6, Count(testCase), 0, First(testCase)), IsOk());
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
      "slug_fill_" + std::to_string(unsigned(testCase)), editor::tests::PixelmatchIdentityParams());
}
}  // namespace donner::gpu::tests
