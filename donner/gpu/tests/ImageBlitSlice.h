#pragma once
/// @file
/// Native image-blit acceptance against CPU sampling and compositing references.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/shader/programs/ImageBlit.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "tiny_skia/Painter.h"
#include "tiny_skia/filter/Blend.h"

namespace donner::gpu::tests {
namespace image_blit_slice {

using Pixel = std::array<uint8_t, 4>;
using FloatPixel = std::array<float, 4>;
inline constexpr uint32_t kRowBytes = 256;
inline constexpr Pixel kPremultiplied{48, 112, 32, 160};
inline constexpr Pixel kStraight{128, 64, 192, 128};
inline constexpr Pixel kMask{64, 128, 32, 192};
inline constexpr Pixel kBackdrop{128, 32, 64, 208};
inline constexpr std::array<Pixel, 4> kCorners{Pixel{32, 128, 224, 255}, Pixel{224, 32, 128, 255},
                                               Pixel{128, 224, 32, 255}, Pixel{224, 128, 32, 255}};

/// Independent texture, alpha, mask and CSS compositing scenarios.
enum class Case {
  Nearest,
  Linear,
  Pixelated,
  Cropped,
  Straight,
  Premultiplied,
  LuminanceMask,
  AlphaMask,
  MaskBounds,
  PathClip,
  Blend
};

struct Scenario {
  Case kind;
  uint32_t blendMode = 0;
  uint32_t width() const { return kind == Case::Pixelated ? 7 : 8; }
  uint32_t height() const { return kind == Case::Pixelated ? 5 : 4; }
  bool textured() const { return kind <= Case::Cropped; }
};

inline Pixel SourcePixel(const Scenario& scenario, uint32_t x, uint32_t y) {
  return scenario.textured()               ? kCorners[y * 2 + x]
         : scenario.kind == Case::Straight ? kStraight
                                           : kPremultiplied;
}

inline uint8_t ClipValue(uint32_t x, uint32_t y) {
  constexpr std::array<uint8_t, 4> values{0, 85, 170, 255};
  return values[(x + y) % values.size()];
}

inline shader::programs::ImageBlitParams Parameters(const Scenario& scenario) {
  shader::programs::ImageBlitParams result{};
  result.mvp[0] = 2.0f / scenario.width();
  result.mvp[5] = -2.0f / scenario.height();
  result.mvp[10] = result.mvp[15] = 1.0f;
  result.mvp[12] = -1.0f;
  result.mvp[13] = 1.0f;
  result.destRect[2] = result.targetSize[0] = float(scenario.width());
  result.destRect[3] = result.targetSize[1] = float(scenario.height());
  result.srcRect[2] = result.srcRect[3] = 1.0f;
  result.opacity = scenario.textured() ? 1.0f : 0.75f;
  result.sourceIsPremult = scenario.kind != Case::Straight;
  result.samplingMode = scenario.kind == Case::Nearest     ? 1u
                        : scenario.kind == Case::Pixelated ? 2u
                                                           : 0u;
  result.pixelatedScale[0] = scenario.width() / 2.0f;
  result.pixelatedScale[1] = scenario.height() / 2.0f;
  result.maskMode = scenario.kind == Case::LuminanceMask                                    ? 1u
                    : scenario.kind == Case::AlphaMask || scenario.kind == Case::MaskBounds ? 2u
                                                                                            : 0u;
  result.applyMaskBounds = scenario.kind == Case::MaskBounds;
  result.maskBounds[0] = 2.0f;
  result.maskBounds[1] = 1.0f;
  result.maskBounds[2] = 6.0f;
  result.maskBounds[3] = 3.0f;
  result.hasClipMask = scenario.kind == Case::PathClip;
  result.blendMode = scenario.blendMode;
  if (scenario.kind == Case::Cropped) {
    const std::array<float, 4> dest{1, 1, 7, 3}, src{0.25f, 0.25f, 0.75f, 0.75f};
    std::copy(dest.begin(), dest.end(), result.destRect);
    std::copy(src.begin(), src.end(), result.srcRect);
  }
  return result;
}

/// Samples the independently expanded CPU image, with clamp-to-edge addressing.
inline FloatPixel Sample(const Scenario& scenario, float u, float v) {
  const uint32_t repeatX = scenario.kind == Case::Pixelated ? 4u : 1u;
  const uint32_t repeatY = scenario.kind == Case::Pixelated ? 3u : 1u;
  const int width = 2 * repeatX, height = 2 * repeatY;
  std::vector<Pixel> expanded(width * height);
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x)
      expanded[y * width + x] = SourcePixel(scenario, x / repeatX, y / repeatY);
  const auto texel = [&](int x, int y) {
    return expanded[std::clamp(y, 0, height - 1) * width + std::clamp(x, 0, width - 1)];
  };
  FloatPixel result{};
  if (scenario.kind == Case::Nearest) {
    const Pixel value = texel(int(std::floor(u * width)), int(std::floor(v * height)));
    for (size_t c = 0; c < 4; ++c) result[c] = value[c] / 255.0f;
    return result;
  }
  const float x = u * width - 0.5f, y = v * height - 0.5f;
  const int left = int(std::floor(x)), top = int(std::floor(y));
  const float fx = x - left, fy = y - top;
  for (int row = 0; row < 2; ++row) {
    for (int column = 0; column < 2; ++column) {
      const Pixel value = texel(left + column, top + row);
      const float weight = (column ? fx : 1.0f - fx) * (row ? fy : 1.0f - fy);
      for (size_t c = 0; c < 4; ++c) result[c] += value[c] / 255.0f * weight;
    }
  }
  return result;
}

/// Uses the raster blend implementation's CSS luminance coefficients for nonseparable modes.
inline std::vector<uint8_t> NonseparableExpected(const Scenario& scenario,
                                                 const tiny_skia::filter::FloatPixmap& background,
                                                 const tiny_skia::filter::FloatPixmap& foreground) {
  constexpr std::array modes{tiny_skia::BlendMode::Hue, tiny_skia::BlendMode::Saturation,
                             tiny_skia::BlendMode::Color, tiny_skia::BlendMode::Luminosity};
  auto target = background.toPixmap();
  const auto source = foreground.toPixmap();
  auto view = target.mutableView();
  tiny_skia::PixmapPaint paint;
  paint.blendMode = modes[scenario.blendMode - 12];
  paint.forceHqPipeline = true;
  tiny_skia::Painter::drawPixmap(view, 0, 0, source.view(), paint);
  return {target.data().begin(), target.data().end()};
}

inline bool OutsideRectangle(float x, float y, const std::array<float, 4>& rect) {
  return x < rect[0] || x >= rect[2] || y < rect[1] || y >= rect[3];
}

inline float CoverageScale(const shader::programs::ImageBlitParams& params, uint32_t x,
                           uint32_t y) {
  float scale = params.opacity;
  if (params.hasClipMask) scale *= ClipValue(x, y) / 255.0f;
  if (params.maskMode == 1)
    scale *= (0.2126f * kMask[0] + 0.7152f * kMask[1] + 0.0722f * kMask[2]) / 255.0f;
  if (params.maskMode == 2) scale *= kMask[3] / 255.0f;
  return scale;
}

inline FloatPixel ForegroundPixel(const Scenario& scenario,
                                  const shader::programs::ImageBlitParams& params, uint32_t x,
                                  uint32_t y) {
  const float px = x + 0.5f, py = y + 0.5f;
  if (OutsideRectangle(
          px, py, {params.destRect[0], params.destRect[1], params.destRect[2], params.destRect[3]}))
    return {};
  if (params.applyMaskBounds && OutsideRectangle(px, py, {2, 1, 6, 3})) return {};
  const float u = params.srcRect[0] + (px - params.destRect[0]) /
                                          (params.destRect[2] - params.destRect[0]) *
                                          (params.srcRect[2] - params.srcRect[0]);
  const float v = params.srcRect[1] + (py - params.destRect[1]) /
                                          (params.destRect[3] - params.destRect[1]) *
                                          (params.srcRect[3] - params.srcRect[1]);
  FloatPixel value = Sample(scenario, u, v);
  if (!params.sourceIsPremult)
    for (size_t c = 0; c < 3; ++c) value[c] *= value[3];
  const float scale = CoverageScale(params, x, y);
  for (float& component : value) component *= scale;
  return value;
}

inline std::vector<uint8_t> Expected(const Scenario& scenario) {
  using tiny_skia::filter::BlendMode;
  using tiny_skia::filter::FloatPixmap;
  const auto params = Parameters(scenario);
  auto foreground = FloatPixmap::fromSize(scenario.width(), scenario.height()).value();
  auto background = FloatPixmap::fromSize(scenario.width(), scenario.height()).value();
  auto output = FloatPixmap::fromSize(scenario.width(), scenario.height()).value();
  for (uint32_t y = 0; y < scenario.height(); ++y) {
    for (uint32_t x = 0; x < scenario.width(); ++x) {
      const size_t offset = (y * scenario.width() + x) * 4;
      if (scenario.kind == Case::Blend)
        for (size_t c = 0; c < 4; ++c) background.data()[offset + c] = kBackdrop[c] / 255.0f;
      const FloatPixel value = ForegroundPixel(scenario, params, x, y);
      std::copy(value.begin(), value.end(), foreground.data().begin() + offset);
    }
  }
  constexpr std::array modes{
      BlendMode::Normal,    BlendMode::Multiply,   BlendMode::Screen,     BlendMode::Overlay,
      BlendMode::Darken,    BlendMode::Lighten,    BlendMode::ColorDodge, BlendMode::ColorBurn,
      BlendMode::HardLight, BlendMode::SoftLight,  BlendMode::Difference, BlendMode::Exclusion,
      BlendMode::Hue,       BlendMode::Saturation, BlendMode::Color,      BlendMode::Luminosity};
  if (scenario.blendMode >= 12) return NonseparableExpected(scenario, background, foreground);
  tiny_skia::filter::blend(background, foreground, output, modes[scenario.blendMode]);
  const auto pixels = output.toPixmap();
  return {pixels.data().begin(), pixels.data().end()};
}

}  // namespace image_blit_slice

/// Renders a textured quad and compares every pixel with independent CPU references.
/// @param device Native backend under test. @param shader Frozen selected or test projections.
/// @param readbackBuffer Backend's bounded buffer readback. @param scenario Reference scenario.
template <typename DeviceType, typename Readback>
void CheckImageBlit(DeviceType& device, const shader::CompiledShaderView& shader,
                    Readback readbackBuffer, image_blit_slice::Scenario scenario) {
  using namespace image_blit_slice;
  ASSERT_THAT(shader.entryPoints, testing::SizeIs(2));
  auto module = device.createShaderModule(
      shader::MakeShaderDescriptor(shader, device.shaderSourceKind(), "image blit"));
  ASSERT_THAT(module, HasResult());
  auto layout = device.createBindGroupLayout({"image", shader::MakeBindingLayout(shader)});
  ASSERT_THAT(layout, HasResult());
  auto pipelineLayout = device.createPipelineLayout({"image", {layout.result()}});
  ASSERT_THAT(pipelineLayout, HasResult());
  const BlendState sourceOver{
      {BlendFactor::One, BlendFactor::OneMinusSrcAlpha, BlendOperation::Add},
      {BlendFactor::One, BlendFactor::OneMinusSrcAlpha, BlendOperation::Add}};
  const std::optional<BlendState> blend =
      scenario.blendMode == 0 ? std::optional<BlendState>{sourceOver} : std::nullopt;
  auto pipeline = device.createRenderPipeline(
      {"image",
       pipelineLayout.result(),
       {module.result(), RcString(shader.entryPoints[0].name.view()), {}},
       FragmentState{module.result(),
                     RcString(shader.entryPoints[1].name.view()),
                     {{TextureFormat::RGBA8Unorm, blend}}},
       PrimitiveTopology::TriangleList,
       CullMode::None});
  ASSERT_THAT(pipeline, HasResult());
  auto output = device.createTexture({"image output",
                                      {scenario.width(), scenario.height()},
                                      TextureFormat::RGBA8Unorm,
                                      TextureUsage::RenderAttachment | TextureUsage::CopySrc});
  ASSERT_THAT(output, HasResult());
  auto outputView = device.createTextureView(output.result(), {"image output"});
  ASSERT_THAT(outputView, HasResult());
  std::vector<Texture> textures;
  std::vector<TextureView> views;
  std::vector<BindGroupEntry> entries;
  constexpr std::array names{"imageTexture", "maskTexture", "dstSnapshotTexture",
                             "clipMaskTexture"};
  for (size_t i = 0; i < names.size(); ++i) {
    const uint32_t width = i == 3 ? scenario.width() : 2u;
    const uint32_t height = i == 3 ? scenario.height() : 2u;
    auto texture = device.createTexture({names[i],
                                         {width, height},
                                         TextureFormat::RGBA8Unorm,
                                         TextureUsage::Sampled | TextureUsage::CopyDst});
    ASSERT_THAT(texture, HasResult());
    auto view = device.createTextureView(texture.result(), {names[i]});
    ASSERT_THAT(view, HasResult());
    std::vector<uint8_t> upload(kRowBytes * height);
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        Pixel pixel = i == 0 ? SourcePixel(scenario, x, y) : i == 1 ? kMask : kBackdrop;
        if (i == 3) pixel.fill(ClipValue(x, y));
        std::copy(pixel.begin(), pixel.end(), upload.begin() + y * kRowBytes + x * 4);
      }
    }
    ASSERT_THAT(
        device.writeTexture(texture.result(), upload, {0, kRowBytes, height}, {width, height}),
        IsOk());
    ASSERT_NE(shader.resource(names[i]), nullptr);
    entries.push_back({shader.resource(names[i])->binding, TextureViewBinding{view.result()}});
    textures.push_back(std::move(texture).result());
    views.push_back(std::move(view).result());
  }
  const FilterMode filter =
      scenario.kind == Case::Nearest ? FilterMode::Nearest : FilterMode::Linear;
  auto sampler = device.createSampler({"image sampler", filter, filter});
  ASSERT_THAT(sampler, HasResult());
  ASSERT_NE(shader.resource("imageSampler"), nullptr);
  entries.push_back({shader.resource("imageSampler")->binding, SamplerBinding{sampler.result()}});
  const auto params = Parameters(scenario);
  auto uniforms = device.createBuffer(
      {"image uniforms", sizeof(params), BufferUsage::Uniform | BufferUsage::CopyDst});
  ASSERT_THAT(uniforms, HasResult());
  ASSERT_THAT(device.writeBuffer(uniforms.result(), 0,
                                 {reinterpret_cast<const uint8_t*>(&params), sizeof(params)}),
              IsOk());
  ASSERT_NE(shader.resource("uniforms"), nullptr);
  entries.push_back(
      {shader.resource("uniforms")->binding, BufferBinding{uniforms.result(), 0, sizeof(params)}});
  auto group = device.createBindGroup({"image", layout.result(), entries});
  ASSERT_THAT(group, HasResult());
  auto readback = device.createBuffer({"image readback", kRowBytes * scenario.height(),
                                       BufferUsage::CopyDst | BufferUsage::MapRead});
  ASSERT_THAT(readback, HasResult());
  auto encoder = device.createCommandEncoder();
  ASSERT_THAT(encoder, HasResult());
  std::array<double, 4> clear{};
  if (scenario.kind == Case::Blend)
    clear = {kBackdrop[0] / 255.0, kBackdrop[1] / 255.0, kBackdrop[2] / 255.0,
             kBackdrop[3] / 255.0};
  auto pass = encoder.result()->beginRenderPass(
      {"image", {{outputView.result(), LoadOp::Clear, StoreOp::Store, clear}}});
  ASSERT_THAT(pass, HasResult());
  ASSERT_THAT(pass.result()->setPipeline(pipeline.result()), IsOk());
  ASSERT_THAT(pass.result()->setBindGroup(0, group.result()), IsOk());
  ASSERT_THAT(pass.result()->draw(6, 1, 0, 0), IsOk());
  ASSERT_THAT(pass.result()->end(), IsOk());
  ASSERT_THAT(encoder.result()->copyTextureToBuffer({output.result()}, readback.result(),
                                                    {0, kRowBytes, scenario.height()},
                                                    {scenario.width(), scenario.height()}),
              IsOk());
  auto commands = encoder.result()->finish();
  ASSERT_THAT(commands, HasResult());
  auto serial = device.submit(std::move(commands).result());
  ASSERT_THAT(serial, HasResult());
  ASSERT_THAT(device.waitForSerial(serial.result(), 5.0), testing::IsTrue());
  const auto bytes = readbackBuffer(readback.result());
  ASSERT_THAT(bytes, HasResult());
  ASSERT_THAT(bytes.result(), testing::SizeIs(testing::Ge(kRowBytes * scenario.height())));
  std::vector<uint8_t> pixels(scenario.width() * scenario.height() * 4);
  for (uint32_t y = 0; y < scenario.height(); ++y)
    std::memcpy(pixels.data() + y * scenario.width() * 4, bytes.result().data() + y * kRowBytes,
                scenario.width() * 4);
  editor::tests::CompareBitmapToBitmap(
      svg::RendererBitmap{Vector2i(scenario.width(), scenario.height()), pixels,
                          scenario.width() * 4},
      svg::RendererBitmap{Vector2i(scenario.width(), scenario.height()), Expected(scenario),
                          scenario.width() * 4},
      "image_blit_" + std::to_string(static_cast<unsigned>(scenario.kind)) + "_" +
          std::to_string(scenario.blendMode),
      editor::tests::PixelmatchIdentityParams());
}

}  // namespace donner::gpu::tests
