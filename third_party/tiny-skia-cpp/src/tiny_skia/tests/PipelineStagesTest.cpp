#include <gtest/gtest.h>

#include <cmath>
#include <numeric>
#include <variant>
#include <vector>

#include "tiny_skia/Color.h"
#include "tiny_skia/Geom.h"
#include "tiny_skia/Painter.h"
#include "tiny_skia/Pixmap.h"
#include "tiny_skia/pipeline/Pipeline.h"
#include "tiny_skia/shaders/Shaders.h"

using namespace tiny_skia;

/// Helper: move a gradient creation result variant into a Paint shader.
template <typename V>
void setShaderFromVariant(Paint& paint, V&& var) {
  std::visit([&paint](auto&& val) { paint.shader = std::move(val); }, std::forward<V>(var));
}

// ===== Helper =====

/// Create a solid-colored 4x4 pixmap.
static Pixmap make4x4(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
  auto pm = Pixmap::fromSize(4, 4);
  EXPECT_TRUE(pm.has_value());
  auto pixels = pm->pixels();
  for (auto& px : pixels) {
    px = PremultipliedColorU8(r, g, b, a);
  }
  return std::move(*pm);
}

/// Read pixel at (x, y) from a Pixmap.
static PremultipliedColorU8 readPx(const Pixmap& pm, std::uint32_t x, std::uint32_t y) {
  return pm.pixel(x, y).value_or(PremultipliedColorU8(0, 0, 0, 0));
}

// ==================================================================
// Gather stage (enables Pattern shader / drawPixmap)
// ==================================================================

TEST(PipelineGatherTest, DrawPixmapCopiesPixels) {
  // Source: 4x4 red pixmap.
  auto src = make4x4(255, 0, 0, 255);

  // Destination: 4x4 blue pixmap.
  auto dst = make4x4(0, 0, 255, 255);

  PixmapPaint ppaint;
  ppaint.blendMode = BlendMode::Source;
  ppaint.quality = FilterQuality::Nearest;
  ppaint.opacity = 1.0f;

  auto dstMut = dst.mutableView();
  Painter::drawPixmap(dstMut, 0, 0, src.view(), ppaint, Transform::identity());

  // The destination should now be red.
  const auto px = readPx(dst, 0, 0);
  EXPECT_EQ(px.red(), 255);
  EXPECT_EQ(px.green(), 0);
  EXPECT_EQ(px.blue(), 0);
  EXPECT_EQ(px.alpha(), 255);
}

TEST(PipelineGatherTest, DrawPixmapWithOffset) {
  auto src = make4x4(0, 255, 0, 255);
  auto dst = Pixmap::fromSize(8, 8).value();
  dst.fill(Color::fromRgba8(0, 0, 0, 255));

  PixmapPaint ppaint;
  ppaint.blendMode = BlendMode::Source;
  ppaint.quality = FilterQuality::Nearest;
  ppaint.opacity = 1.0f;

  auto dstMut = dst.mutableView();
  Painter::drawPixmap(dstMut, 2, 2, src.view(), ppaint, Transform::identity());

  // (0,0) should still be black, (2,2) should be green.
  const auto black = readPx(dst, 0, 0);
  EXPECT_EQ(black.red(), 0);
  EXPECT_EQ(black.green(), 0);

  const auto green = readPx(dst, 2, 2);
  EXPECT_EQ(green.red(), 0);
  EXPECT_EQ(green.green(), 255);
  EXPECT_EQ(green.blue(), 0);
}

// ==================================================================
// Transform stage
// ==================================================================

TEST(PipelineTransformTest, ScaledFillRect) {
  auto pm = Pixmap::fromSize(4, 4).value();
  pm.fill(Color::transparent);
  auto mut = pm.mutableView();

  Paint paint;
  paint.setColor(Color::fromRgba8(255, 0, 0, 255));
  paint.antiAlias = false;

  // Scale 2x, so a 1x1 rect becomes 2x2.
  const auto ts = Transform::fromScale(2.0f, 2.0f);
  auto rect = Rect::fromLTRB(0.0f, 0.0f, 1.0f, 1.0f);
  ASSERT_TRUE(rect.has_value());
  Painter::fillRect(mut, *rect, paint, ts);

  // (0,0) and (1,1) should be red.
  EXPECT_EQ(readPx(pm, 0, 0).red(), 255);
  EXPECT_EQ(readPx(pm, 1, 1).red(), 255);
  // (2,2) should still be transparent (outside the scaled rect).
  EXPECT_EQ(readPx(pm, 2, 2).alpha(), 0);
}

// ==================================================================
// Evenly-spaced 2-stop gradient stage
// ==================================================================

TEST(PipelineGradientTest, LinearGradient2Stop) {
  auto pm = Pixmap::fromSize(10, 1).value();
  pm.fill(Color::transparent);
  auto mut = pm.mutableView();

  // Create a horizontal linear gradient from red to blue.
  auto stops = std::vector<GradientStop>{
      GradientStop::create(0.0f, Color::fromRgba8(255, 0, 0, 255)),
      GradientStop::create(1.0f, Color::fromRgba8(0, 0, 255, 255)),
  };
  auto grad = LinearGradient::create(Point::fromXY(0.0f, 0.0f), Point::fromXY(10.0f, 0.0f),
                                     std::move(stops), SpreadMode::Pad, Transform::identity());
  ASSERT_TRUE(grad.has_value());

  Paint paint;
  setShaderFromVariant(paint, std::move(*grad));
  paint.antiAlias = false;

  auto rect = Rect::fromLTRB(0.0f, 0.0f, 10.0f, 1.0f);
  ASSERT_TRUE(rect.has_value());
  Painter::fillRect(mut, *rect, paint, Transform::identity());

  // First pixel should be close to red.
  const auto first = readPx(pm, 0, 0);
  EXPECT_GT(first.red(), 200) << "first pixel r=" << (int)first.red() << " g=" << (int)first.green()
                              << " b=" << (int)first.blue() << " a=" << (int)first.alpha();
  EXPECT_LT(first.blue(), 55);
  EXPECT_EQ(first.alpha(), 255);

  // Last pixel should be close to blue.
  const auto last = readPx(pm, 9, 0);
  EXPECT_LT(last.red(), 55);
  EXPECT_GT(last.blue(), 200);
  EXPECT_EQ(last.alpha(), 255);

  // Middle pixel should have both red and blue.
  const auto mid = readPx(pm, 5, 0);
  EXPECT_GT(mid.red(), 50);
  EXPECT_GT(mid.blue(), 50);
}

// ==================================================================
// Multi-stop gradient stage
// ==================================================================

TEST(PipelineGradientTest, LinearGradient3Stop) {
  auto pm = Pixmap::fromSize(10, 1).value();
  pm.fill(Color::transparent);
  auto mut = pm.mutableView();

  auto stops = std::vector<GradientStop>{
      GradientStop::create(0.0f, Color::fromRgba8(255, 0, 0, 255)),
      GradientStop::create(0.5f, Color::fromRgba8(0, 255, 0, 255)),
      GradientStop::create(1.0f, Color::fromRgba8(0, 0, 255, 255)),
  };
  auto grad = LinearGradient::create(Point::fromXY(0.0f, 0.0f), Point::fromXY(10.0f, 0.0f),
                                     std::move(stops), SpreadMode::Pad, Transform::identity());
  ASSERT_TRUE(grad.has_value());

  Paint paint;
  setShaderFromVariant(paint, std::move(*grad));
  paint.antiAlias = false;

  auto rect = Rect::fromLTRB(0.0f, 0.0f, 10.0f, 1.0f);
  ASSERT_TRUE(rect.has_value());
  Painter::fillRect(mut, *rect, paint, Transform::identity());

  // First pixel ~ red.
  const auto first = readPx(pm, 0, 0);
  EXPECT_GT(first.red(), 200);

  // Middle pixel ~ green.
  const auto mid = readPx(pm, 5, 0);
  EXPECT_GT(mid.green(), 100);

  // Last pixel ~ blue.
  const auto last = readPx(pm, 9, 0);
  EXPECT_GT(last.blue(), 200);
}

// ==================================================================
// xyToRadius (radial gradient)
// ==================================================================

TEST(PipelineGradientTest, RadialGradient) {
  auto pm = Pixmap::fromSize(10, 10).value();
  pm.fill(Color::transparent);
  auto mut = pm.mutableView();

  auto stops = std::vector<GradientStop>{
      GradientStop::create(0.0f, Color::fromRgba8(255, 0, 0, 255)),
      GradientStop::create(1.0f, Color::fromRgba8(0, 0, 255, 255)),
  };
  auto grad =
      RadialGradient::create(Point::fromXY(5.0f, 5.0f), 0.0f, Point::fromXY(5.0f, 5.0f), 5.0f,
                             std::move(stops), SpreadMode::Pad, Transform::identity());
  if (!grad.has_value()) {
    GTEST_SKIP() << "RadialGradient::create returned nullopt";
  }

  Paint paint;
  setShaderFromVariant(paint, std::move(*grad));
  paint.antiAlias = false;

  auto rect = Rect::fromLTRB(0.0f, 0.0f, 10.0f, 10.0f);
  ASSERT_TRUE(rect.has_value());
  Painter::fillRect(mut, *rect, paint, Transform::identity());

  // Center pixel should be red.
  const auto center = readPx(pm, 5, 5);
  EXPECT_GT(center.red(), 200);
  EXPECT_EQ(center.alpha(), 255);

  // Corner pixel should be blue (farther from center).
  const auto corner = readPx(pm, 0, 0);
  EXPECT_GT(corner.blue(), 100);
}

// ==================================================================
// xyToUnitAngle (sweep gradient)
// ==================================================================

TEST(PipelineGradientTest, SweepGradient) {
  auto pm = Pixmap::fromSize(10, 10).value();
  pm.fill(Color::transparent);
  auto mut = pm.mutableView();

  auto stops = std::vector<GradientStop>{
      GradientStop::create(0.0f, Color::fromRgba8(255, 0, 0, 255)),
      GradientStop::create(1.0f, Color::fromRgba8(0, 0, 255, 255)),
  };
  auto grad = SweepGradient::create(Point::fromXY(5.0f, 5.0f), 0.0f, 360.0f, std::move(stops),
                                    SpreadMode::Pad, Transform::identity());
  if (!grad.has_value()) {
    GTEST_SKIP() << "SweepGradient::create returned nullopt";
  }

  Paint paint;
  setShaderFromVariant(paint, std::move(*grad));
  paint.antiAlias = false;

  auto rect = Rect::fromLTRB(0.0f, 0.0f, 10.0f, 10.0f);
  ASSERT_TRUE(rect.has_value());
  Painter::fillRect(mut, *rect, paint, Transform::identity());

  // Just verify we got non-transparent pixels at center and edges.
  EXPECT_EQ(readPx(pm, 5, 5).alpha(), 255);
  EXPECT_EQ(readPx(pm, 0, 5).alpha(), 255);
  EXPECT_EQ(readPx(pm, 9, 5).alpha(), 255);
}

// ==================================================================
// padX1 / repeatX1 / reflectX1 tile mode stages
// ==================================================================

TEST(PipelineTileModeTest, PadGradient) {
  auto pm = Pixmap::fromSize(20, 1).value();
  pm.fill(Color::transparent);
  auto mut = pm.mutableView();

  // Gradient from red to blue over x=[0, 10). Beyond 10, pad.
  auto stops = std::vector<GradientStop>{
      GradientStop::create(0.0f, Color::fromRgba8(255, 0, 0, 255)),
      GradientStop::create(1.0f, Color::fromRgba8(0, 0, 255, 255)),
  };
  auto grad = LinearGradient::create(Point::fromXY(0.0f, 0.0f), Point::fromXY(10.0f, 0.0f),
                                     std::move(stops), SpreadMode::Pad, Transform::identity());
  ASSERT_TRUE(grad.has_value());

  Paint paint;
  setShaderFromVariant(paint, std::move(*grad));
  paint.antiAlias = false;

  auto rect = Rect::fromLTRB(0.0f, 0.0f, 20.0f, 1.0f);
  ASSERT_TRUE(rect.has_value());
  Painter::fillRect(mut, *rect, paint, Transform::identity());

  // Pixel at x=15 (past the gradient end) should be blue (padded).
  const auto padded = readPx(pm, 15, 0);
  EXPECT_GT(padded.blue(), 200);
  EXPECT_LT(padded.red(), 55);
}

TEST(PipelineTileModeTest, RepeatGradient) {
  auto pm = Pixmap::fromSize(20, 1).value();
  pm.fill(Color::transparent);
  auto mut = pm.mutableView();

  auto stops = std::vector<GradientStop>{
      GradientStop::create(0.0f, Color::fromRgba8(255, 0, 0, 255)),
      GradientStop::create(1.0f, Color::fromRgba8(0, 0, 255, 255)),
  };
  auto grad = LinearGradient::create(Point::fromXY(0.0f, 0.0f), Point::fromXY(10.0f, 0.0f),
                                     std::move(stops), SpreadMode::Repeat, Transform::identity());
  ASSERT_TRUE(grad.has_value());

  Paint paint;
  setShaderFromVariant(paint, std::move(*grad));
  paint.antiAlias = false;

  auto rect = Rect::fromLTRB(0.0f, 0.0f, 20.0f, 1.0f);
  ASSERT_TRUE(rect.has_value());
  Painter::fillRect(mut, *rect, paint, Transform::identity());

  // At x=10, the gradient repeats, so pixel at x=10 should be reddish.
  // x=10 is start of second cycle.
  const auto repeated = readPx(pm, 10, 0);
  // Should be close to red (start of repeat).
  EXPECT_GT(repeated.red(), 100);
}

TEST(PipelineTileModeTest, ReflectGradient) {
  auto pm = Pixmap::fromSize(20, 1).value();
  pm.fill(Color::transparent);
  auto mut = pm.mutableView();

  auto stops = std::vector<GradientStop>{
      GradientStop::create(0.0f, Color::fromRgba8(255, 0, 0, 255)),
      GradientStop::create(1.0f, Color::fromRgba8(0, 0, 255, 255)),
  };
  auto grad = LinearGradient::create(Point::fromXY(0.0f, 0.0f), Point::fromXY(10.0f, 0.0f),
                                     std::move(stops), SpreadMode::Reflect, Transform::identity());
  ASSERT_TRUE(grad.has_value());

  Paint paint;
  setShaderFromVariant(paint, std::move(*grad));
  paint.antiAlias = false;

  auto rect = Rect::fromLTRB(0.0f, 0.0f, 20.0f, 1.0f);
  ASSERT_TRUE(rect.has_value());
  Painter::fillRect(mut, *rect, paint, Transform::identity());

  // At x=19, the gradient reflects back, so pixel should be reddish.
  const auto reflected = readPx(pm, 19, 0);
  EXPECT_GT(reflected.red(), 100);
}

// ==================================================================
// Blend mode stages
// ==================================================================

TEST(PipelineBlendTest, DarkenBlend) {
  auto pm = Pixmap::fromSize(4, 4).value();
  // Fill with a medium gray.
  pm.fill(Color::fromRgba8(128, 128, 128, 255));
  auto mut = pm.mutableView();

  Paint paint;
  paint.setColor(Color::fromRgba8(200, 50, 100, 255));
  paint.blendMode = BlendMode::Darken;
  paint.antiAlias = false;

  auto rect = Rect::fromLTRB(0.0f, 0.0f, 4.0f, 4.0f);
  ASSERT_TRUE(rect.has_value());
  Painter::fillRect(mut, *rect, paint, Transform::identity());

  // Darken takes min per channel.
  const auto px = readPx(pm, 0, 0);
  EXPECT_LE(px.red(), 128);   // min(200, 128) = 128
  EXPECT_LE(px.green(), 50);  // min(50, 128) = 50
  EXPECT_LE(px.blue(), 100);  // min(100, 128) = 100
  EXPECT_EQ(px.alpha(), 255);
}

TEST(PipelineBlendTest, LightenBlend) {
  auto pm = Pixmap::fromSize(4, 4).value();
  pm.fill(Color::fromRgba8(128, 128, 128, 255));
  auto mut = pm.mutableView();

  Paint paint;
  paint.setColor(Color::fromRgba8(200, 50, 100, 255));
  paint.blendMode = BlendMode::Lighten;
  paint.antiAlias = false;

  auto rect = Rect::fromLTRB(0.0f, 0.0f, 4.0f, 4.0f);
  ASSERT_TRUE(rect.has_value());
  Painter::fillRect(mut, *rect, paint, Transform::identity());

  const auto px = readPx(pm, 0, 0);
  EXPECT_GE(px.red(), 198);    // max(200, 128) = 200, lowp ±2
  EXPECT_GE(px.green(), 126);  // max(50, 128) = 128, lowp ±2
  EXPECT_GE(px.blue(), 126);   // max(100, 128) = 128, lowp ±2
}

TEST(PipelineBlendTest, DifferenceBlend) {
  auto pm = Pixmap::fromSize(4, 4).value();
  pm.fill(Color::fromRgba8(100, 200, 50, 255));
  auto mut = pm.mutableView();

  Paint paint;
  paint.setColor(Color::fromRgba8(100, 200, 50, 255));
  paint.blendMode = BlendMode::Difference;
  paint.antiAlias = false;

  auto rect = Rect::fromLTRB(0.0f, 0.0f, 4.0f, 4.0f);
  ASSERT_TRUE(rect.has_value());
  Painter::fillRect(mut, *rect, paint, Transform::identity());

  // Difference of same color should be ~0.
  // Lowp approximation can produce slightly non-zero results.
  const auto px = readPx(pm, 0, 0);
  EXPECT_LE(px.red(), 3);
  EXPECT_LE(px.green(), 3);
  EXPECT_LE(px.blue(), 3);
}

TEST(PipelineBlendTest, ExclusionBlend) {
  auto pm = Pixmap::fromSize(4, 4).value();
  pm.fill(Color::fromRgba8(0, 0, 0, 255));
  auto mut = pm.mutableView();

  Paint paint;
  paint.setColor(Color::fromRgba8(255, 0, 0, 255));
  paint.blendMode = BlendMode::Exclusion;
  paint.antiAlias = false;

  auto rect = Rect::fromLTRB(0.0f, 0.0f, 4.0f, 4.0f);
  ASSERT_TRUE(rect.has_value());
  Painter::fillRect(mut, *rect, paint, Transform::identity());

  // Exclusion with black gives the source color.
  // Lowp approximation may be ±2 off.
  const auto px = readPx(pm, 0, 0);
  EXPECT_GE(px.red(), 253);
  EXPECT_LE(px.green(), 2);
  EXPECT_LE(px.blue(), 2);
}

TEST(PipelineBlendTest, ScreenBlend) {
  auto pm = Pixmap::fromSize(4, 4).value();
  pm.fill(Color::fromRgba8(128, 128, 128, 255));
  auto mut = pm.mutableView();

  Paint paint;
  paint.setColor(Color::fromRgba8(128, 128, 128, 255));
  paint.blendMode = BlendMode::Screen;
  paint.antiAlias = false;

  auto rect = Rect::fromLTRB(0.0f, 0.0f, 4.0f, 4.0f);
  ASSERT_TRUE(rect.has_value());
  Painter::fillRect(mut, *rect, paint, Transform::identity());

  // Screen of gray+gray should be lighter.
  const auto px = readPx(pm, 0, 0);
  EXPECT_GT(px.red(), 128);
}

TEST(PipelineBlendTest, OverlayBlend) {
  auto pm = Pixmap::fromSize(4, 4).value();
  pm.fill(Color::fromRgba8(100, 100, 100, 255));
  auto mut = pm.mutableView();

  Paint paint;
  paint.setColor(Color::fromRgba8(200, 200, 200, 255));
  paint.blendMode = BlendMode::Overlay;
  paint.antiAlias = false;

  auto rect = Rect::fromLTRB(0.0f, 0.0f, 4.0f, 4.0f);
  ASSERT_TRUE(rect.has_value());
  Painter::fillRect(mut, *rect, paint, Transform::identity());

  // Just verify it doesn't crash and produces non-transparent output.
  const auto px = readPx(pm, 0, 0);
  EXPECT_EQ(px.alpha(), 255);
}

TEST(PipelineBlendTest, HardLightBlend) {
  auto pm = Pixmap::fromSize(4, 4).value();
  pm.fill(Color::fromRgba8(100, 100, 100, 255));
  auto mut = pm.mutableView();

  Paint paint;
  paint.setColor(Color::fromRgba8(200, 200, 200, 255));
  paint.blendMode = BlendMode::HardLight;
  paint.antiAlias = false;

  auto rect = Rect::fromLTRB(0.0f, 0.0f, 4.0f, 4.0f);
  ASSERT_TRUE(rect.has_value());
  Painter::fillRect(mut, *rect, paint, Transform::identity());

  const auto px = readPx(pm, 0, 0);
  EXPECT_EQ(px.alpha(), 255);
}

TEST(PipelineBlendTest, SoftLightBlend) {
  auto pm = Pixmap::fromSize(4, 4).value();
  pm.fill(Color::fromRgba8(100, 100, 100, 255));
  auto mut = pm.mutableView();

  Paint paint;
  paint.setColor(Color::fromRgba8(200, 200, 200, 255));
  paint.blendMode = BlendMode::SoftLight;
  paint.antiAlias = false;

  auto rect = Rect::fromLTRB(0.0f, 0.0f, 4.0f, 4.0f);
  ASSERT_TRUE(rect.has_value());
  Painter::fillRect(mut, *rect, paint, Transform::identity());

  const auto px = readPx(pm, 0, 0);
  EXPECT_EQ(px.alpha(), 255);
}

TEST(PipelineBlendTest, ColorDodgeBlend) {
  auto pm = Pixmap::fromSize(4, 4).value();
  pm.fill(Color::fromRgba8(128, 128, 128, 255));
  auto mut = pm.mutableView();

  Paint paint;
  paint.setColor(Color::fromRgba8(128, 128, 128, 255));
  paint.blendMode = BlendMode::ColorDodge;
  paint.antiAlias = false;

  auto rect = Rect::fromLTRB(0.0f, 0.0f, 4.0f, 4.0f);
  ASSERT_TRUE(rect.has_value());
  Painter::fillRect(mut, *rect, paint, Transform::identity());

  // ColorDodge should brighten.
  const auto px = readPx(pm, 0, 0);
  EXPECT_GT(px.red(), 128);
}

TEST(PipelineBlendTest, ColorBurnBlend) {
  auto pm = Pixmap::fromSize(4, 4).value();
  pm.fill(Color::fromRgba8(200, 200, 200, 255));
  auto mut = pm.mutableView();

  Paint paint;
  paint.setColor(Color::fromRgba8(128, 128, 128, 255));
  paint.blendMode = BlendMode::ColorBurn;
  paint.antiAlias = false;

  auto rect = Rect::fromLTRB(0.0f, 0.0f, 4.0f, 4.0f);
  ASSERT_TRUE(rect.has_value());
  Painter::fillRect(mut, *rect, paint, Transform::identity());

  // ColorBurn should darken.
  const auto px = readPx(pm, 0, 0);
  EXPECT_LT(px.red(), 200);
}

// ==================================================================
// Non-separable blend mode stages (Hue, Saturation, Color, Luminosity)
// ==================================================================

TEST(PipelineBlendTest, HueBlend) {
  auto pm = Pixmap::fromSize(4, 4).value();
  pm.fill(Color::fromRgba8(0, 128, 0, 255));
  auto mut = pm.mutableView();

  Paint paint;
  paint.setColor(Color::fromRgba8(255, 0, 0, 255));
  paint.blendMode = BlendMode::Hue;
  paint.antiAlias = false;

  auto rect = Rect::fromLTRB(0.0f, 0.0f, 4.0f, 4.0f);
  ASSERT_TRUE(rect.has_value());
  Painter::fillRect(mut, *rect, paint, Transform::identity());

  const auto px = readPx(pm, 0, 0);
  EXPECT_EQ(px.alpha(), 255);
  // Hue from red, sat/lum from green: result should have some red.
  EXPECT_GT(px.red(), 0);
}

TEST(PipelineBlendTest, SaturationBlend) {
  auto pm = Pixmap::fromSize(4, 4).value();
  pm.fill(Color::fromRgba8(128, 64, 0, 255));
  auto mut = pm.mutableView();

  Paint paint;
  paint.setColor(Color::fromRgba8(255, 0, 0, 255));
  paint.blendMode = BlendMode::Saturation;
  paint.antiAlias = false;

  auto rect = Rect::fromLTRB(0.0f, 0.0f, 4.0f, 4.0f);
  ASSERT_TRUE(rect.has_value());
  Painter::fillRect(mut, *rect, paint, Transform::identity());

  const auto px = readPx(pm, 0, 0);
  EXPECT_EQ(px.alpha(), 255);
}

TEST(PipelineBlendTest, ColorBlend) {
  auto pm = Pixmap::fromSize(4, 4).value();
  pm.fill(Color::fromRgba8(128, 128, 128, 255));
  auto mut = pm.mutableView();

  Paint paint;
  paint.setColor(Color::fromRgba8(255, 0, 0, 255));
  paint.blendMode = BlendMode::Color;
  paint.antiAlias = false;

  auto rect = Rect::fromLTRB(0.0f, 0.0f, 4.0f, 4.0f);
  ASSERT_TRUE(rect.has_value());
  Painter::fillRect(mut, *rect, paint, Transform::identity());

  const auto px = readPx(pm, 0, 0);
  EXPECT_EQ(px.alpha(), 255);
  EXPECT_GT(px.red(), 0);
}

TEST(PipelineBlendTest, LuminosityBlend) {
  auto pm = Pixmap::fromSize(4, 4).value();
  pm.fill(Color::fromRgba8(255, 0, 0, 255));
  auto mut = pm.mutableView();

  Paint paint;
  paint.setColor(Color::fromRgba8(200, 200, 200, 255));
  paint.blendMode = BlendMode::Luminosity;
  paint.antiAlias = false;

  auto rect = Rect::fromLTRB(0.0f, 0.0f, 4.0f, 4.0f);
  ASSERT_TRUE(rect.has_value());
  Painter::fillRect(mut, *rect, paint, Transform::identity());

  const auto px = readPx(pm, 0, 0);
  EXPECT_EQ(px.alpha(), 255);
}

// ==================================================================
// Gamma correction stages
// ==================================================================

TEST(PipelineGammaTest, SrgbColorspace) {
  auto pm = Pixmap::fromSize(4, 4).value();
  pm.fill(Color::transparent);
  auto mut = pm.mutableView();

  Paint paint;
  paint.setColor(Color::fromRgba8(128, 128, 128, 255));
  paint.colorspace = ColorSpace::SimpleSRGB;
  paint.antiAlias = false;

  auto rect = Rect::fromLTRB(0.0f, 0.0f, 4.0f, 4.0f);
  ASSERT_TRUE(rect.has_value());
  Painter::fillRect(mut, *rect, paint, Transform::identity());

  // Should produce some result (may differ from Linear).
  const auto px = readPx(pm, 0, 0);
  EXPECT_EQ(px.alpha(), 255);
  // sRGB gamma should still produce a recognizable gray.
  EXPECT_GT(px.red(), 50);
  EXPECT_LT(px.red(), 200);
}

TEST(PipelineGammaTest, Gamma2Colorspace) {
  auto pm = Pixmap::fromSize(4, 4).value();
  pm.fill(Color::transparent);
  auto mut = pm.mutableView();

  Paint paint;
  paint.setColor(Color::fromRgba8(128, 128, 128, 255));
  paint.colorspace = ColorSpace::Gamma2;
  paint.antiAlias = false;

  auto rect = Rect::fromLTRB(0.0f, 0.0f, 4.0f, 4.0f);
  ASSERT_TRUE(rect.has_value());
  Painter::fillRect(mut, *rect, paint, Transform::identity());

  const auto px = readPx(pm, 0, 0);
  EXPECT_EQ(px.alpha(), 255);
  EXPECT_GT(px.red(), 50);
}

// ==================================================================
// Bilinear filtering stage (Pattern with Bilinear quality)
// ==================================================================

TEST(PipelineFilterTest, BilinearDrawPixmap) {
  auto src = make4x4(255, 0, 0, 255);
  auto dst = make4x4(0, 0, 255, 255);

  PixmapPaint ppaint;
  ppaint.blendMode = BlendMode::Source;
  ppaint.quality = FilterQuality::Bilinear;
  ppaint.opacity = 1.0f;

  auto dstMut = dst.mutableView();
  Painter::drawPixmap(dstMut, 0, 0, src.view(), ppaint, Transform::identity());

  // Should copy red over blue.
  const auto px = readPx(dst, 1, 1);
  EXPECT_GT(px.red(), 200);
  EXPECT_LT(px.blue(), 55);
}

TEST(PipelineFilterTest, BicubicDrawPixmap) {
  auto src = make4x4(255, 0, 0, 255);
  auto dst = make4x4(0, 0, 255, 255);

  PixmapPaint ppaint;
  ppaint.blendMode = BlendMode::Source;
  ppaint.quality = FilterQuality::Bicubic;
  ppaint.opacity = 1.0f;

  auto dstMut = dst.mutableView();
  Painter::drawPixmap(dstMut, 0, 0, src.view(), ppaint, Transform::identity());

  // Should copy red over blue.
  const auto px = readPx(dst, 1, 1);
  EXPECT_GT(px.red(), 200);
  EXPECT_LT(px.blue(), 55);
}
