#pragma once
/// @file
/// Native dedicated-gradient rendering against geometric and straight-alpha references.
#include <optional>

#include "donner/gpu/shader/programs/SlugGradient.h"
#include "donner/gpu/tests/SlugMaskSlice.h"
#include "tiny_skia/filter/FloatPixmap.h"

namespace donner::gpu::tests {
namespace slug_gradient_slice {
inline constexpr uint32_t kWidth = 8, kHeight = 8, kRowBytes = 256;
enum class Case {
  Linear,
  Binary,
  Reflect,
  Repeat,
  Transformed,
  TransparentStops,
  ManyStops,
  EmptyStops,
  SingleStop,
  Radial,
  Focal,
  FocalRadius,
  RadialOutside,
  RadialDegenerate,
  ZeroRadius,
  ClipMask,
  ClipPolygon,
  EvenOdd,
  DeclaredRange
};
struct Stop {
  float offset;
  std::array<float, 4> color;
};
inline std::vector<Stop> Stops(Case testCase) {
  if (testCase == Case::EmptyStops) {
    return {};
  }
  const uint32_t count = testCase == Case::ManyStops ? 16u : testCase == Case::SingleStop ? 1u : 2u;
  std::vector<Stop> result;
  for (uint32_t i = 0; i < count; ++i) {
    const float t = count > 1 ? float(i) / float(count - 1) : 0.0f;
    const float alpha = testCase == Case::TransparentStops ? 0.25f + 0.5f * t : 1.0f;
    const float channel = testCase == Case::ManyStops ? float((i * 37) % 160) : 64.0f * t;
    result.push_back(
        {t, {(32 + channel) / 255, (64 + channel) / 255, (96 + channel) / 255, alpha}});
  }
  return result;
}
inline slug_mask_slice::Case GeometryCase(Case testCase) {
  if (testCase == Case::EvenOdd) {
    return slug_mask_slice::Case::DoubleEvenOdd;
  }
  if (testCase == Case::DeclaredRange) {
    return slug_mask_slice::Case::DeclaredRange;
  }
  return testCase == Case::Binary ? slug_mask_slice::Case::Binary : slug_mask_slice::Case::Analytic;
}
inline void ConfigureRadial(shader::programs::SlugGradientParams& p, Case testCase) {
  p.gradientKind = testCase >= Case::Radial && testCase <= Case::ZeroRadius;
  p.radialRadius = 8;
  if (testCase == Case::Focal) {
    p.radialFocal[0] = p.radialFocal[1] = 1;
    p.radialCenter[0] = 3;
    p.radialCenter[1] = 2;
    p.radialRadius = 6;
  }
  if (testCase == Case::FocalRadius) {
    p.radialFocalRadius = 2;
  }
  if (testCase == Case::RadialOutside) {
    p.radialCenter[0] = 6;
    p.radialRadius = 2;
  }
  if (testCase == Case::RadialDegenerate) {
    p.radialCenter[0] = 6;
    p.radialRadius = 6;
  }
  if (testCase == Case::ZeroRadius) {
    p.radialRadius = 0;
  }
}
inline void ConfigurePaint(shader::programs::SlugGradientParams& p, Case testCase) {
  p.row0[0] = p.row1[1] = 1;
  p.endGrad[0] = testCase == Case::ManyStops ? 6.0f : 8.0f;
  if (testCase == Case::Reflect || testCase == Case::Repeat) {
    p.spreadMode = testCase == Case::Reflect ? 1u : 2u;
    p.endGrad[0] = 2;
    p.row0[2] = -3;
  }
  if (testCase == Case::Transformed) {
    p.row0[0] = -2;
    p.row0[2] = 10;
  }
  ConfigureRadial(p, testCase);
}
inline shader::programs::SlugGradientParams Parameters(Case testCase) {
  const auto geometry = slug_mask_slice::Parameters(GeometryCase(testCase));
  shader::programs::SlugGradientParams p{};
  std::copy_n(geometry.mvp, 16, p.mvp);
  std::copy_n(geometry.boundingVertices, 16, p.boundingVertices);
  p.viewport[0] = kWidth;
  p.viewport[1] = kHeight;
  p.boundingVertexCount = 4;
  p.gridHBandCount = p.gridVBandCount = 1;
  p.gridHStride = p.gridVStride = 8;
  p.antialias = testCase != Case::Binary && testCase != Case::EvenOdd;
  p.fillRule = testCase == Case::EvenOdd;
  ConfigurePaint(p, testCase);
  p.hasClipMask = testCase == Case::ClipMask;
  p.hasClipPolygon = testCase == Case::ClipPolygon;
  constexpr std::array<float, 16> planes{1, 0, -2, 0, -1, 0, 6, 0, 0, 1, -2, 0, 0, -1, 5, 0};
  std::copy(planes.begin(), planes.end(), p.clipPolygonPlanes);
  const auto stops = Stops(testCase);
  p.stopCount = static_cast<uint32_t>(stops.size());
  for (size_t i = 0; i < stops.size(); ++i) {
    p.stopOffsets[i] = stops[i].offset;
    std::copy(stops[i].color.begin(), stops[i].color.end(), p.stopColors + i * 4);
  }
  return p;
}
inline uint8_t ClipValue(uint32_t x, uint32_t y) {
  constexpr std::array<uint8_t, 4> values{0, 85, 170, 255};
  return values[(x + y) % 4];
}
inline bool OutsideClipPolygon(Case testCase, float x, float y) {
  if (testCase != Case::ClipPolygon) {
    return false;
  }
  return x < 2 || x >= 6 || y < 2 || y >= 5;
}
inline float Coverage(Case testCase, uint32_t x, uint32_t y) {
  const float px = x + 0.5f, py = y + 0.5f;
  if (py < 1 || py >= 6 || testCase == Case::EvenOdd || testCase == Case::DeclaredRange) {
    return 0;
  }
  if (OutsideClipPolygon(testCase, px, py)) {
    return 0;
  }
  float result = testCase == Case::Binary
                     ? float(px >= 1.25f && px < 6.75f)
                     : std::max(0.0f, std::min(float(x + 1), 6.75f) - std::max(float(x), 1.25f));
  if (testCase == Case::ClipMask) {
    result *= ClipValue(x, y) / 255.0f;
  }
  return result;
}
/// Intersects the sample ray with the outer circle, independently of the shader's t polynomial.
inline std::optional<double> RadialParameter(const shader::programs::SlugGradientParams& p,
                                             double x, double y) {
  if (p.radialRadius == 0) {
    return 1.0;
  }
  const double vx = x - p.radialFocal[0], vy = y - p.radialFocal[1];
  const double distance = std::hypot(vx, vy);
  if (p.radialFocalRadius > 0) {
    return (distance - p.radialFocalRadius) / (p.radialRadius - p.radialFocalRadius);
  }
  if (distance == 0) {
    return 0;
  }
  const double dx = p.radialCenter[0] - p.radialFocal[0], dy = p.radialCenter[1] - p.radialFocal[1];
  const double projected = (dx * vx + dy * vy) / distance;
  const double discriminant =
      double(p.radialRadius) * p.radialRadius - dx * dx - dy * dy + projected * projected;
  if (discriminant < 0) {
    return std::nullopt;
  }
  const double nearDistance = projected - std::sqrt(discriminant);
  const double farDistance = projected + std::sqrt(discriminant);
  const double hit = nearDistance > 0 ? nearDistance : farDistance;
  if (hit <= 0) {
    return std::nullopt;
  }
  return distance / hit;
}
inline double Spread(double t, uint32_t mode) {
  if (mode == 0) {
    return std::clamp(t, 0.0, 1.0);
  }
  const double period = mode == 1 ? 2.0 : 1.0;
  double result = std::fmod(t, period);
  if (result < 0) {
    result += period;
  }
  return result > 1 ? 2 - result : result;
}
inline std::array<float, 4> SampleStops(const std::vector<Stop>& stops, double t) {
  if (stops.empty()) {
    return {};
  }
  if (t <= stops.front().offset) {
    return stops.front().color;
  }
  if (t >= stops.back().offset) {
    return stops.back().color;
  }
  const auto right =
      std::upper_bound(stops.begin(), stops.end(), t,
                       [](double value, const Stop& stop) { return value < stop.offset; });
  const auto& left = *(right - 1);
  const double amount = (t - left.offset) / (right->offset - left.offset);
  std::array<float, 4> result{};
  for (size_t i = 0; i < 4; ++i) {
    result[i] = std::lerp(double(left.color[i]), double(right->color[i]), amount);
  }
  return result;
}
inline std::vector<uint8_t> Expected(Case testCase) {
  const auto p = Parameters(testCase);
  const auto stops = Stops(testCase);
  auto image = tiny_skia::filter::FloatPixmap::fromSize(kWidth, kHeight).value();
  for (uint32_t y = 0; y < kHeight; ++y) {
    for (uint32_t x = 0; x < kWidth; ++x) {
      const double px = p.row0[0] * (x + 0.5) + p.row0[1] * (y + 0.5) + p.row0[2];
      const double py = p.row1[0] * (x + 0.5) + p.row1[1] * (y + 0.5) + p.row1[2];
      const auto parameter =
          p.gradientKind ? RadialParameter(p, px, py) : std::optional<double>{px / p.endGrad[0]};
      if (!parameter) {
        continue;
      }
      auto color = SampleStops(stops, Spread(*parameter, p.spreadMode));
      for (size_t i = 0; i < 3; ++i) {
        color[i] *= color[3];
      }
      const float coverage = Coverage(testCase, x, y);
      for (size_t i = 0; i < 4; ++i) {
        image.data()[(y * kWidth + x) * 4 + i] = color[i] * coverage;
      }
    }
  }
  const auto pixels = image.toPixmap();
  return {pixels.data().begin(), pixels.data().end()};
}
}  // namespace slug_gradient_slice

/// Renders the dedicated gradient program and checks every channel using the repository bitmap
/// comparator.
/// @param device Native backend under test. @param shader Frozen selected or test projections.
/// @param readbackBuffer Backend's bounded buffer readback. @param testCase Reference case.
template <typename DeviceType, typename Readback>
void CheckSlugGradient(DeviceType& device, const shader::CompiledShaderView& shader,
                       Readback readbackBuffer, slug_gradient_slice::Case testCase) {
  using namespace slug_gradient_slice;
  ASSERT_THAT(shader.entryPoints, testing::SizeIs(2));
  auto module = device.createShaderModule(
      shader::MakeShaderDescriptor(shader, device.shaderSourceKind(), "Slug gradient"));
  ASSERT_THAT(module, HasResult());
  auto layout = device.createBindGroupLayout({"Slug gradient", shader::MakeBindingLayout(shader)});
  ASSERT_THAT(layout, HasResult());
  auto pipelineLayout = device.createPipelineLayout({"Slug gradient", {layout.result()}});
  ASSERT_THAT(pipelineLayout, HasResult());
  const BlendState over{{BlendFactor::One, BlendFactor::OneMinusSrcAlpha, BlendOperation::Add},
                        {BlendFactor::One, BlendFactor::OneMinusSrcAlpha, BlendOperation::Add}};
  auto pipeline = device.createRenderPipeline(
      {"Slug gradient",
       pipelineLayout.result(),
       {module.result(), RcString(shader.entryPoints[0].name.view()), {}},
       FragmentState{module.result(),
                     RcString(shader.entryPoints[1].name.view()),
                     {{TextureFormat::RGBA8Unorm, over}}},
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
  ASSERT_EQ(
      slug_mask_slice::UploadBinding(device, shader, "uniforms",
                                     {reinterpret_cast<const uint8_t*>(&params), sizeof(params)},
                                     sizeof(params), buffers, entries),
      true);
  ASSERT_EQ(
      slug_mask_slice::UploadGeometry(device, shader, GeometryCase(testCase), buffers, entries),
      true);
  ASSERT_NE(shader.resource("clipMaskTexture"), nullptr);
  entries.push_back(
      {shader.resource("clipMaskTexture")->binding, TextureViewBinding{clipView.result()}});
  auto bindGroup = device.createBindGroup({"Slug gradient", layout.result(), entries});
  ASSERT_THAT(bindGroup, HasResult());
  auto readback = device.createBuffer(
      {"mask readback", kRowBytes * kHeight, BufferUsage::CopyDst | BufferUsage::MapRead});
  ASSERT_THAT(readback, HasResult());
  auto encoder = device.createCommandEncoder();
  ASSERT_THAT(encoder, HasResult());
  auto pass = encoder.result()->beginRenderPass(
      {"Slug gradient", {{outputView.result(), LoadOp::Clear, StoreOp::Store, {0, 0, 0, 0}}}});
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
  // The ClipMask case differs by one alpha LSB on two pixels between the GPU
  // float pipeline and the CPU oracle (measured: RGB bit-identical, alpha 127
  // vs 128). Pixelmatch 1.x masked this through uint8 blend quantization; 2.0
  // compares at full precision (PR #1285). The 2px allowance at threshold 0.0
  // keeps every other pixel bit-exact; all other cases still count zero.
  editor::tests::CompareBitmapToBitmap(
      svg::RendererBitmap{Vector2i(kWidth, kHeight), pixels, kWidth * 4},
      svg::RendererBitmap{Vector2i(kWidth, kHeight), Expected(testCase), kWidth * 4},
      "slug_gradient_" + std::to_string(static_cast<unsigned>(testCase)),
      editor::tests::ApprovedPixelToleranceParams(0.0f, 2, /*includeAntiAliasing=*/true));
}

}  // namespace donner::gpu::tests
