#include <gtest/gtest.h>

#include <cmath>

#include "tiny_skia/Color.h"
#include "tiny_skia/Math.h"
#include "tiny_skia/Point.h"
#include "tiny_skia/pipeline/Pipeline.h"
#include "tiny_skia/shaders/Shaders.h"

namespace {

using tiny_skia::Color;
using tiny_skia::ColorSpace;
using tiny_skia::Gradient;
using tiny_skia::GradientStop;
using tiny_skia::LinearGradient;
using tiny_skia::NormalizedF32;
using tiny_skia::Pattern;
using tiny_skia::Point;
using tiny_skia::RadialGradient;
using tiny_skia::Shader;
using tiny_skia::SpreadMode;
using tiny_skia::SweepGradient;
using tiny_skia::Transform;

// ---------------------------------------------------------------------------
// Transform tests — matches Rust transform.rs #[cfg(test)]
// ---------------------------------------------------------------------------

TEST(TransformTest, IdentityIsIdentity) {
  const auto ts = Transform::identity();
  EXPECT_TRUE(ts.isIdentity());
  EXPECT_TRUE(ts.isFinite());
  EXPECT_FALSE(ts.hasScale());
  EXPECT_FALSE(ts.hasSkew());
  EXPECT_FALSE(ts.hasTranslate());
  EXPECT_TRUE(ts.isTranslate());
  EXPECT_TRUE(ts.isScaleTranslate());
}

TEST(TransformTest, FromScaleClassification) {
  const auto ts = Transform::fromScale(2.0f, 3.0f);
  EXPECT_FALSE(ts.isIdentity());
  EXPECT_TRUE(ts.hasScale());
  EXPECT_FALSE(ts.hasSkew());
  EXPECT_FALSE(ts.hasTranslate());
  EXPECT_TRUE(ts.isScaleTranslate());
  EXPECT_FALSE(ts.isTranslate());
}

TEST(TransformTest, FromScaleOneIsNotScale) {
  const auto ts = Transform::fromScale(1.0f, 1.0f);
  EXPECT_TRUE(ts.isIdentity());
  EXPECT_FALSE(ts.hasScale());
}

TEST(TransformTest, FromTranslateClassification) {
  const auto ts = Transform::fromTranslate(5.0f, 6.0f);
  EXPECT_FALSE(ts.isIdentity());
  EXPECT_FALSE(ts.hasScale());
  EXPECT_FALSE(ts.hasSkew());
  EXPECT_TRUE(ts.hasTranslate());
  EXPECT_TRUE(ts.isTranslate());
  EXPECT_TRUE(ts.isScaleTranslate());
}

TEST(TransformTest, FromRowWithSkew) {
  const auto ts = Transform::fromRow(1.0f, 3.0f, 2.0f, 1.0f, 0.0f, 0.0f);
  EXPECT_TRUE(ts.hasSkew());
  EXPECT_FALSE(ts.hasScale());
  EXPECT_FALSE(ts.isScaleTranslate());
}

TEST(TransformTest, InvertIdentity) {
  const auto inv = Transform::identity().invert();
  ASSERT_TRUE(inv.has_value());
  EXPECT_TRUE(inv->isIdentity());
}

TEST(TransformTest, InvertScale) {
  const auto ts = Transform::fromScale(2.0f, 4.0f);
  const auto inv = ts.invert();
  ASSERT_TRUE(inv.has_value());
  EXPECT_FLOAT_EQ(inv->sx, 0.5f);
  EXPECT_FLOAT_EQ(inv->sy, 0.25f);
}

TEST(TransformTest, InvertSingularReturnsNullopt) {
  const auto ts = Transform::fromScale(0.0f, 0.0f);
  EXPECT_FALSE(ts.invert().has_value());
}

TEST(TransformTest, PreScale) {
  // preScale(2.0, -4.0)
  const auto ts = Transform::fromRow(1.2f, 3.4f, -5.6f, -7.8f, 1.2f, 3.4f);
  const auto result = ts.preScale(2.0f, -4.0f);
  EXPECT_NEAR(result.sx, 2.4f, 1e-5f);
  EXPECT_NEAR(result.ky, 6.8f, 1e-5f);
  EXPECT_NEAR(result.kx, 22.4f, 1e-5f);
  EXPECT_NEAR(result.sy, 31.2f, 1e-5f);
  EXPECT_NEAR(result.tx, 1.2f, 1e-5f);
  EXPECT_NEAR(result.ty, 3.4f, 1e-5f);
}

TEST(TransformTest, PostScale) {
  // postScale(2.0, -4.0)
  const auto ts = Transform::fromRow(1.2f, 3.4f, -5.6f, -7.8f, 1.2f, 3.4f);
  const auto result = ts.postScale(2.0f, -4.0f);
  EXPECT_NEAR(result.sx, 2.4f, 1e-5f);
  EXPECT_NEAR(result.ky, -13.6f, 1e-5f);
  EXPECT_NEAR(result.kx, -11.2f, 1e-5f);
  EXPECT_NEAR(result.sy, 31.2f, 1e-5f);
  EXPECT_NEAR(result.tx, 2.4f, 1e-5f);
  EXPECT_NEAR(result.ty, -13.6f, 1e-5f);
}

TEST(TransformTest, PreTranslate) {
  const auto ts = Transform::fromScale(2.0f, 3.0f);
  const auto result = ts.preTranslate(10.0f, 20.0f);
  EXPECT_FLOAT_EQ(result.sx, 2.0f);
  EXPECT_FLOAT_EQ(result.sy, 3.0f);
  EXPECT_FLOAT_EQ(result.tx, 20.0f);
  EXPECT_FLOAT_EQ(result.ty, 60.0f);
}

TEST(TransformTest, PostTranslate) {
  const auto ts = Transform::fromScale(2.0f, 3.0f);
  const auto result = ts.postTranslate(10.0f, 20.0f);
  EXPECT_FLOAT_EQ(result.sx, 2.0f);
  EXPECT_FLOAT_EQ(result.sy, 3.0f);
  EXPECT_FLOAT_EQ(result.tx, 10.0f);
  EXPECT_FLOAT_EQ(result.ty, 20.0f);
}

TEST(TransformTest, PreConcatIdentity) {
  const auto ts = Transform::fromRow(1.2f, 3.4f, -5.6f, -7.8f, 1.2f, 3.4f);
  const auto result = ts.preConcat(Transform::identity());
  EXPECT_EQ(result, ts);
}

TEST(TransformTest, PostConcatIdentity) {
  const auto ts = Transform::fromRow(1.2f, 3.4f, -5.6f, -7.8f, 1.2f, 3.4f);
  const auto result = ts.postConcat(Transform::identity());
  EXPECT_EQ(result, ts);
}

TEST(TransformTest, InvertRoundTrip) {
  const auto ts = Transform::fromRow(1.2f, 3.4f, -5.6f, -7.8f, 1.2f, 3.4f);
  const auto inv = ts.invert();
  ASSERT_TRUE(inv.has_value());
  const auto roundTrip = ts.postConcat(*inv);
  EXPECT_NEAR(roundTrip.sx, 1.0f, 1e-4f);
  EXPECT_NEAR(roundTrip.sy, 1.0f, 1e-4f);
  EXPECT_NEAR(roundTrip.kx, 0.0f, 1e-4f);
  EXPECT_NEAR(roundTrip.ky, 0.0f, 1e-4f);
  EXPECT_NEAR(roundTrip.tx, 0.0f, 1e-4f);
  EXPECT_NEAR(roundTrip.ty, 0.0f, 1e-4f);
}

// ---------------------------------------------------------------------------
// GradientStop tests
// ---------------------------------------------------------------------------

TEST(GradientStopTest, CreateClamps) {
  const auto stop = GradientStop::create(1.5f, Color::white);
  EXPECT_EQ(stop.position.get(), 1.0f);

  const auto stop2 = GradientStop::create(-0.5f, Color::black);
  EXPECT_EQ(stop2.position.get(), 0.0f);
}

TEST(GradientStopTest, CreateNormal) {
  const auto stop = GradientStop::create(0.5f, Color::white);
  EXPECT_FLOAT_EQ(stop.position.get(), 0.5f);
}

// ---------------------------------------------------------------------------
// LinearGradient::create edge cases
// ---------------------------------------------------------------------------

TEST(LinearGradientTest, CreateRejectsEmptyStops) {
  const auto result = LinearGradient::create(Point::fromXY(0, 0), Point::fromXY(100, 0), {},
                                             SpreadMode::Pad, Transform::identity());
  EXPECT_FALSE(result.has_value());
}

TEST(LinearGradientTest, CreateSingleStopReturnsSolidColor) {
  const auto c = Color::fromRgba8(50, 127, 150, 200);
  const auto result = LinearGradient::create(Point::fromXY(0, 0), Point::fromXY(100, 0),
                                             {GradientStop::create(0.5f, c)}, SpreadMode::Pad,
                                             Transform::identity());
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(std::holds_alternative<Color>(*result));
  EXPECT_EQ(std::get<Color>(*result), c);
}

TEST(LinearGradientTest, CreateDegeneratePadReturnsLastColor) {
  const auto c0 = Color::fromRgba8(50, 127, 150, 200);
  const auto c1 = Color::fromRgba8(220, 140, 75, 180);
  const auto result =
      LinearGradient::create(Point::fromXY(0, 0), Point::fromXY(0, 0),  // degenerate: same point
                             {GradientStop::create(0.0f, c0), GradientStop::create(1.0f, c1)},
                             SpreadMode::Pad, Transform::identity());
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(std::holds_alternative<Color>(*result));
  EXPECT_EQ(std::get<Color>(*result), c1);
}

TEST(LinearGradientTest, CreateDegenerateRepeatReturnsAverage) {
  const auto c0 = Color::fromRgba8(50, 127, 150, 200);
  const auto c1 = Color::fromRgba8(220, 140, 75, 180);
  const auto result =
      LinearGradient::create(Point::fromXY(0, 0), Point::fromXY(0, 0),
                             {GradientStop::create(0.0f, c0), GradientStop::create(1.0f, c1)},
                             SpreadMode::Repeat, Transform::identity());
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(std::holds_alternative<Color>(*result));
  // Average color should be between c0 and c1.
  const auto avg = std::get<Color>(*result);
  EXPECT_GT(avg.red(), 0.0f);
  EXPECT_LT(avg.red(), 1.0f);
}

TEST(LinearGradientTest, CreateDegenerateReflectReturnsAverage) {
  const auto c0 = Color::fromRgba8(50, 127, 150, 200);
  const auto c1 = Color::fromRgba8(220, 140, 75, 180);
  const auto result =
      LinearGradient::create(Point::fromXY(0, 0), Point::fromXY(0, 0),
                             {GradientStop::create(0.0f, c0), GradientStop::create(1.0f, c1)},
                             SpreadMode::Reflect, Transform::identity());
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(std::holds_alternative<Color>(*result));
}

TEST(LinearGradientTest, CreateNonInvertibleTransformReturnsNullopt) {
  const auto c0 = Color::fromRgba8(50, 127, 150, 200);
  const auto c1 = Color::fromRgba8(220, 140, 75, 180);
  const auto result =
      LinearGradient::create(Point::fromXY(0, 0), Point::fromXY(100, 100),
                             {GradientStop::create(0.0f, c0), GradientStop::create(1.0f, c1)},
                             SpreadMode::Pad, Transform::fromScale(0.0f, 0.0f));
  EXPECT_FALSE(result.has_value());
}

TEST(LinearGradientTest, CreateValidTwoStopReturnsGradient) {
  const auto c0 = Color::fromRgba8(50, 127, 150, 200);
  const auto c1 = Color::fromRgba8(220, 140, 75, 180);
  const auto result =
      LinearGradient::create(Point::fromXY(10, 10), Point::fromXY(190, 190),
                             {GradientStop::create(0.0f, c0), GradientStop::create(1.0f, c1)},
                             SpreadMode::Pad, Transform::identity());
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(std::holds_alternative<LinearGradient>(*result));
}

TEST(LinearGradientTest, CreateValidThreeStopReturnsGradient) {
  const auto c0 = Color::fromRgba8(50, 127, 150, 200);
  const auto c1 = Color::fromRgba8(220, 140, 75, 180);
  const auto c2 = Color::fromRgba8(40, 180, 55, 160);
  const auto result =
      LinearGradient::create(Point::fromXY(10, 10), Point::fromXY(190, 190),
                             {GradientStop::create(0.0f, c0), GradientStop::create(0.5f, c1),
                              GradientStop::create(1.0f, c2)},
                             SpreadMode::Pad, Transform::identity());
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(std::holds_alternative<LinearGradient>(*result));
}

TEST(LinearGradientTest, CreateInfinityLengthReturnsNullopt) {
  const auto c0 = Color::white;
  const auto c1 = Color::black;
  const auto result =
      LinearGradient::create(Point::fromXY(0, 0), Point::fromXY(INFINITY, 0),
                             {GradientStop::create(0.0f, c0), GradientStop::create(1.0f, c1)},
                             SpreadMode::Pad, Transform::identity());
  EXPECT_FALSE(result.has_value());
}

// ---------------------------------------------------------------------------
// Gradient opacity
// ---------------------------------------------------------------------------

TEST(LinearGradientTest, OpaqueColorsReportOpaque) {
  const auto result = LinearGradient::create(
      Point::fromXY(0, 0), Point::fromXY(100, 0),
      {GradientStop::create(0.0f, Color::white), GradientStop::create(1.0f, Color::black)},
      SpreadMode::Pad, Transform::identity());
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(std::holds_alternative<LinearGradient>(*result));
  EXPECT_TRUE(std::get<LinearGradient>(*result).isOpaque());
}

TEST(LinearGradientTest, TransparentColorsReportNonOpaque) {
  const auto c0 = Color::fromRgba8(255, 0, 0, 128);
  const auto c1 = Color::fromRgba8(0, 0, 255, 200);
  const auto result =
      LinearGradient::create(Point::fromXY(0, 0), Point::fromXY(100, 0),
                             {GradientStop::create(0.0f, c0), GradientStop::create(1.0f, c1)},
                             SpreadMode::Pad, Transform::identity());
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(std::holds_alternative<LinearGradient>(*result));
  EXPECT_FALSE(std::get<LinearGradient>(*result).isOpaque());
}

// ---------------------------------------------------------------------------
// Shader variant dispatch (free functions)
// ---------------------------------------------------------------------------

TEST(ShaderTest, SolidColorIsOpaque) {
  Shader shader = Color::white;
  EXPECT_TRUE(tiny_skia::isShaderOpaque(shader));
}

TEST(ShaderTest, SolidColorTransparentIsNotOpaque) {
  Shader shader = Color::fromRgba8(255, 0, 0, 128);
  EXPECT_FALSE(tiny_skia::isShaderOpaque(shader));
}

TEST(ShaderTest, LinearGradientOpaqueIsOpaque) {
  const auto result = LinearGradient::create(
      Point::fromXY(0, 0), Point::fromXY(100, 0),
      {GradientStop::create(0.0f, Color::white), GradientStop::create(1.0f, Color::black)},
      SpreadMode::Pad, Transform::identity());
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(std::holds_alternative<LinearGradient>(*result));
  Shader shader = std::get<LinearGradient>(*result);
  EXPECT_TRUE(tiny_skia::isShaderOpaque(shader));
}

TEST(ShaderTest, TransformShaderSolidColorIsNoOp) {
  const auto c = Color::fromRgba8(100, 150, 200, 255);
  Shader shader = c;
  tiny_skia::transformShader(shader, Transform::fromTranslate(10.0f, 20.0f));
  // SolidColor is transform-invariant; should be unchanged.
  EXPECT_EQ(std::get<Color>(shader), c);
}

TEST(ShaderTest, TransformShaderUpdatesGradientTransform) {
  const auto result = LinearGradient::create(
      Point::fromXY(0, 0), Point::fromXY(100, 0),
      {GradientStop::create(0.0f, Color::white), GradientStop::create(1.0f, Color::black)},
      SpreadMode::Pad, Transform::identity());
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(std::holds_alternative<LinearGradient>(*result));
  Shader shader = std::get<LinearGradient>(*result);

  const auto origTransform = std::get<LinearGradient>(shader).base_.transform;
  tiny_skia::transformShader(shader, Transform::fromTranslate(10.0f, 20.0f));
  const auto newTransform = std::get<LinearGradient>(shader).base_.transform;
  // Transform should have changed.
  EXPECT_NE(origTransform, newTransform);
}

TEST(ShaderTest, ApplyOpacitySolidColor) {
  Shader shader = Color::white;
  tiny_skia::applyShaderOpacity(shader, 0.5f);
  const auto& c = std::get<Color>(shader);
  EXPECT_NEAR(c.alpha(), 0.5f, 1e-3f);
  EXPECT_NEAR(c.red(), 1.0f, 1e-3f);
}

TEST(ShaderTest, ApplyOpacityGradient) {
  const auto result = LinearGradient::create(
      Point::fromXY(0, 0), Point::fromXY(100, 0),
      {GradientStop::create(0.0f, Color::white), GradientStop::create(1.0f, Color::black)},
      SpreadMode::Pad, Transform::identity());
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(std::holds_alternative<LinearGradient>(*result));
  Shader shader = std::get<LinearGradient>(*result);

  EXPECT_TRUE(tiny_skia::isShaderOpaque(shader));
  tiny_skia::applyShaderOpacity(shader, 0.5f);
  EXPECT_FALSE(tiny_skia::isShaderOpaque(shader));
}

// ---------------------------------------------------------------------------
// LinearGradient pushStages (pipeline integration)
// ---------------------------------------------------------------------------

TEST(LinearGradientTest, PushStagesTwoStopPad) {
  const auto result =
      LinearGradient::create(Point::fromXY(10, 10), Point::fromXY(190, 190),
                             {GradientStop::create(0.0f, Color::fromRgba8(50, 127, 150, 200)),
                              GradientStop::create(1.0f, Color::fromRgba8(220, 140, 75, 180))},
                             SpreadMode::Pad, Transform::identity());
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(std::holds_alternative<LinearGradient>(*result));

  tiny_skia::pipeline::RasterPipelineBuilder builder;
  const bool ok = std::get<LinearGradient>(*result).pushStages(ColorSpace::Linear, builder);
  EXPECT_TRUE(ok);
  // Should have pushed at least SeedShader + Transform + PadX1 +
  // EvenlySpaced2StopGradient + Premultiply (colors are non-opaque).
  EXPECT_GE(builder.compile().stageCount(), 4u);
}

TEST(LinearGradientTest, PushStagesThreeStopRepeat) {
  const auto result =
      LinearGradient::create(Point::fromXY(10, 10), Point::fromXY(100, 100),
                             {GradientStop::create(0.0f, Color::fromRgba8(50, 127, 150, 200)),
                              GradientStop::create(0.5f, Color::fromRgba8(220, 140, 75, 180)),
                              GradientStop::create(1.0f, Color::fromRgba8(40, 180, 55, 160))},
                             SpreadMode::Repeat, Transform::identity());
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(std::holds_alternative<LinearGradient>(*result));

  tiny_skia::pipeline::RasterPipelineBuilder builder;
  const bool ok = std::get<LinearGradient>(*result).pushStages(ColorSpace::Linear, builder);
  EXPECT_TRUE(ok);
  EXPECT_GE(builder.compile().stageCount(), 4u);
}

TEST(LinearGradientTest, PushStagesOpaqueSkipsPremultiply) {
  const auto result = LinearGradient::create(
      Point::fromXY(0, 0), Point::fromXY(100, 0),
      {GradientStop::create(0.0f, Color::white), GradientStop::create(1.0f, Color::black)},
      SpreadMode::Pad, Transform::identity());
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(std::holds_alternative<LinearGradient>(*result));

  tiny_skia::pipeline::RasterPipelineBuilder builder;
  const bool ok = std::get<LinearGradient>(*result).pushStages(ColorSpace::Linear, builder);
  EXPECT_TRUE(ok);
  // With opaque colors, no Premultiply stage, so fewer stages.
  const auto pipeline = builder.compile();
  EXPECT_GE(pipeline.stageCount(), 3u);
}

// ---------------------------------------------------------------------------
// Gradient with unevenly spaced stops (triggers dummy endpoint insertion)
// ---------------------------------------------------------------------------

TEST(LinearGradientTest, CreateWithOffsetStopsSucceeds) {
  const auto c0 = Color::fromRgba8(50, 127, 150, 200);
  const auto c1 = Color::fromRgba8(220, 140, 75, 180);
  // Stops don't start at 0 or end at 1 → triggers dummy insertion.
  const auto result =
      LinearGradient::create(Point::fromXY(10, 10), Point::fromXY(190, 190),
                             {GradientStop::create(0.25f, c0), GradientStop::create(0.75f, c1)},
                             SpreadMode::Pad, Transform::identity());
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(std::holds_alternative<LinearGradient>(*result));

  // Verify it can push stages without error.
  tiny_skia::pipeline::RasterPipelineBuilder builder;
  const bool ok = std::get<LinearGradient>(*result).pushStages(ColorSpace::Linear, builder);
  EXPECT_TRUE(ok);
}

// ---------------------------------------------------------------------------
// pushShaderStages for SolidColor
// ---------------------------------------------------------------------------

TEST(ShaderTest, PushStagesSolidColor) {
  Shader shader = Color::white;
  tiny_skia::pipeline::RasterPipelineBuilder builder;
  const bool ok = tiny_skia::pushShaderStages(shader, ColorSpace::Linear, builder);
  EXPECT_TRUE(ok);
  EXPECT_GE(builder.compile().stageCount(), 1u);
}

// ---------------------------------------------------------------------------
// SweepGradient tests
// ---------------------------------------------------------------------------

TEST(SweepGradientTest, CreateRejectsEmptyStops) {
  const auto result = SweepGradient::create(Point::fromXY(100, 100), 0.0f, 360.0f, {},
                                            SpreadMode::Pad, Transform::identity());
  EXPECT_FALSE(result.has_value());
}

TEST(SweepGradientTest, CreateSingleStopReturnsSolidColor) {
  const auto c = Color::fromRgba8(50, 127, 150, 200);
  const auto result =
      SweepGradient::create(Point::fromXY(100, 100), 0.0f, 360.0f, {GradientStop::create(0.5f, c)},
                            SpreadMode::Pad, Transform::identity());
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(std::holds_alternative<Color>(*result));
}

TEST(SweepGradientTest, CreateRejectsInvertedAngles) {
  const auto result = SweepGradient::create(
      Point::fromXY(100, 100), 270.0f, 90.0f,
      {GradientStop::create(0.0f, Color::white), GradientStop::create(1.0f, Color::black)},
      SpreadMode::Pad, Transform::identity());
  EXPECT_FALSE(result.has_value());
}

TEST(SweepGradientTest, CreateRejectsNonFiniteAngles) {
  const auto result = SweepGradient::create(
      Point::fromXY(100, 100), 0.0f, INFINITY,
      {GradientStop::create(0.0f, Color::white), GradientStop::create(1.0f, Color::black)},
      SpreadMode::Pad, Transform::identity());
  EXPECT_FALSE(result.has_value());
}

TEST(SweepGradientTest, CreateValidFullCircle) {
  const auto result =
      SweepGradient::create(Point::fromXY(100, 100), 0.0f, 360.0f,
                            {GradientStop::create(0.0f, Color::fromRgba8(50, 127, 150, 200)),
                             GradientStop::create(1.0f, Color::fromRgba8(220, 140, 75, 180))},
                            SpreadMode::Pad, Transform::identity());
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(std::holds_alternative<SweepGradient>(*result));
}

TEST(SweepGradientTest, CreatePartialSweep) {
  const auto result =
      SweepGradient::create(Point::fromXY(100, 100), 135.0f, 225.0f,
                            {GradientStop::create(0.0f, Color::fromRgba8(50, 127, 150, 200)),
                             GradientStop::create(1.0f, Color::fromRgba8(220, 140, 75, 180))},
                            SpreadMode::Pad, Transform::identity());
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(std::holds_alternative<SweepGradient>(*result));
}

TEST(SweepGradientTest, CreateNonInvertibleTransformReturnsNullopt) {
  const auto result = SweepGradient::create(
      Point::fromXY(100, 100), 0.0f, 360.0f,
      {GradientStop::create(0.0f, Color::white), GradientStop::create(1.0f, Color::black)},
      SpreadMode::Pad, Transform::fromScale(0.0f, 0.0f));
  EXPECT_FALSE(result.has_value());
}

TEST(SweepGradientTest, PushStagesSucceeds) {
  const auto result = SweepGradient::create(
      Point::fromXY(100, 100), 0.0f, 360.0f,
      {GradientStop::create(0.0f, Color::white), GradientStop::create(1.0f, Color::black)},
      SpreadMode::Pad, Transform::identity());
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(std::holds_alternative<SweepGradient>(*result));

  tiny_skia::pipeline::RasterPipelineBuilder builder;
  const bool ok = std::get<SweepGradient>(*result).pushStages(ColorSpace::Linear, builder);
  EXPECT_TRUE(ok);
  EXPECT_GE(builder.compile().stageCount(), 3u);
}

TEST(SweepGradientTest, PushStagesPartialSweepHasScaleBias) {
  const auto result = SweepGradient::create(
      Point::fromXY(100, 100), 135.0f, 225.0f,
      {GradientStop::create(0.0f, Color::white), GradientStop::create(1.0f, Color::black)},
      SpreadMode::Pad, Transform::identity());
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(std::holds_alternative<SweepGradient>(*result));

  tiny_skia::pipeline::RasterPipelineBuilder builder;
  const bool ok = std::get<SweepGradient>(*result).pushStages(ColorSpace::Linear, builder);
  EXPECT_TRUE(ok);
  // Partial sweep needs XYToUnitAngle + ApplyConcentricScaleBias.
  EXPECT_GE(builder.compile().stageCount(), 4u);
}

TEST(SweepGradientTest, ShaderVariantDispatch) {
  const auto result = SweepGradient::create(
      Point::fromXY(100, 100), 0.0f, 360.0f,
      {GradientStop::create(0.0f, Color::white), GradientStop::create(1.0f, Color::black)},
      SpreadMode::Pad, Transform::identity());
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(std::holds_alternative<SweepGradient>(*result));

  Shader shader = std::get<SweepGradient>(*result);
  EXPECT_TRUE(tiny_skia::isShaderOpaque(shader));

  tiny_skia::pipeline::RasterPipelineBuilder builder;
  EXPECT_TRUE(tiny_skia::pushShaderStages(shader, ColorSpace::Linear, builder));

  tiny_skia::transformShader(shader, Transform::fromTranslate(5.0f, 5.0f));
  tiny_skia::applyShaderOpacity(shader, 0.5f);
  EXPECT_FALSE(tiny_skia::isShaderOpaque(shader));
}

// ---------------------------------------------------------------------------
// RadialGradient tests
// ---------------------------------------------------------------------------

TEST(RadialGradientTest, CreateRejectsEmptyStops) {
  const auto result = RadialGradient::create(Point::fromXY(100, 100), 0.0f, Point::fromXY(100, 100),
                                             100.0f, {}, SpreadMode::Pad, Transform::identity());
  EXPECT_FALSE(result.has_value());
}

TEST(RadialGradientTest, CreateSingleStopReturnsSolidColor) {
  const auto c = Color::fromRgba8(50, 127, 150, 200);
  const auto result = RadialGradient::create(Point::fromXY(100, 100), 0.0f, Point::fromXY(100, 100),
                                             100.0f, {GradientStop::create(0.5f, c)},
                                             SpreadMode::Pad, Transform::identity());
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(std::holds_alternative<Color>(*result));
}

TEST(RadialGradientTest, CreateRejectsNegativeRadius) {
  const auto result = RadialGradient::create(
      Point::fromXY(100, 100), -1.0f, Point::fromXY(100, 100), 100.0f,
      {GradientStop::create(0.0f, Color::white), GradientStop::create(1.0f, Color::black)},
      SpreadMode::Pad, Transform::identity());
  EXPECT_FALSE(result.has_value());
}

TEST(RadialGradientTest, CreateSimpleRadial) {
  // Simple: same center, start radius = 0.
  const auto result =
      RadialGradient::create(Point::fromXY(100, 100), 0.0f, Point::fromXY(100, 100), 100.0f,
                             {GradientStop::create(0.0f, Color::fromRgba8(50, 127, 150, 200)),
                              GradientStop::create(1.0f, Color::fromRgba8(220, 140, 75, 180))},
                             SpreadMode::Pad, Transform::identity());
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(std::holds_alternative<RadialGradient>(*result));
}

TEST(RadialGradientTest, CreateConcentricRadial) {
  // Concentric: same center, different non-zero radii.
  const auto result =
      RadialGradient::create(Point::fromXY(100, 100), 30.0f, Point::fromXY(100, 100), 90.0f,
                             {GradientStop::create(0.0f, Color::fromRgba8(50, 127, 150, 200)),
                              GradientStop::create(1.0f, Color::fromRgba8(220, 140, 75, 180))},
                             SpreadMode::Pad, Transform::identity());
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(std::holds_alternative<RadialGradient>(*result));
}

TEST(RadialGradientTest, CreateTwoPointConical) {
  // Two-point conical: different centers, different radii.
  const auto result =
      RadialGradient::create(Point::fromXY(100, 100), 0.0f, Point::fromXY(120, 80), 100.0f,
                             {GradientStop::create(0.0f, Color::fromRgba8(50, 127, 150, 200)),
                              GradientStop::create(1.0f, Color::fromRgba8(220, 140, 75, 180))},
                             SpreadMode::Pad, Transform::identity());
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(std::holds_alternative<RadialGradient>(*result));
}

TEST(RadialGradientTest, CreateStripGradient) {
  // Strip: different centers, same radius.
  const auto result =
      RadialGradient::create(Point::fromXY(50, 100), 50.0f, Point::fromXY(150, 100), 50.0f,
                             {GradientStop::create(0.0f, Color::fromRgba8(50, 127, 150, 200)),
                              GradientStop::create(1.0f, Color::fromRgba8(220, 140, 75, 180))},
                             SpreadMode::Pad, Transform::identity());
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(std::holds_alternative<RadialGradient>(*result));
}

TEST(RadialGradientTest, CreateNonInvertibleTransformReturnsNullopt) {
  const auto result = RadialGradient::create(
      Point::fromXY(100, 100), 0.0f, Point::fromXY(100, 100), 100.0f,
      {GradientStop::create(0.0f, Color::white), GradientStop::create(1.0f, Color::black)},
      SpreadMode::Pad, Transform::fromScale(0.0f, 0.0f));
  EXPECT_FALSE(result.has_value());
}

TEST(RadialGradientTest, CreateDegenerateSameCenterSameRadius) {
  // Both center and radii degenerate with Pad mode and radius > threshold.
  const auto result = RadialGradient::create(
      Point::fromXY(100, 100), 50.0f, Point::fromXY(100, 100), 50.0f,
      {GradientStop::create(0.0f, Color::white), GradientStop::create(1.0f, Color::black)},
      SpreadMode::Pad, Transform::identity());
  // With Pad and radius > threshold, creates a hard-stop gradient.
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(std::holds_alternative<RadialGradient>(*result));
}

TEST(RadialGradientTest, PushStagesSimpleRadial) {
  const auto result = RadialGradient::create(
      Point::fromXY(100, 100), 0.0f, Point::fromXY(100, 100), 100.0f,
      {GradientStop::create(0.0f, Color::white), GradientStop::create(1.0f, Color::black)},
      SpreadMode::Pad, Transform::identity());
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(std::holds_alternative<RadialGradient>(*result));

  tiny_skia::pipeline::RasterPipelineBuilder builder;
  const bool ok = std::get<RadialGradient>(*result).pushStages(ColorSpace::Linear, builder);
  EXPECT_TRUE(ok);
  EXPECT_GE(builder.compile().stageCount(), 3u);
}

TEST(RadialGradientTest, PushStagesTwoPointConical) {
  const auto result =
      RadialGradient::create(Point::fromXY(100, 100), 0.0f, Point::fromXY(120, 80), 100.0f,
                             {GradientStop::create(0.0f, Color::fromRgba8(50, 127, 150, 200)),
                              GradientStop::create(1.0f, Color::fromRgba8(220, 140, 75, 180))},
                             SpreadMode::Pad, Transform::identity());
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(std::holds_alternative<RadialGradient>(*result));

  tiny_skia::pipeline::RasterPipelineBuilder builder;
  const bool ok = std::get<RadialGradient>(*result).pushStages(ColorSpace::Linear, builder);
  EXPECT_TRUE(ok);
  EXPECT_GE(builder.compile().stageCount(), 4u);
}

TEST(RadialGradientTest, ShaderVariantDispatch) {
  const auto result = RadialGradient::create(
      Point::fromXY(100, 100), 0.0f, Point::fromXY(100, 100), 100.0f,
      {GradientStop::create(0.0f, Color::white), GradientStop::create(1.0f, Color::black)},
      SpreadMode::Pad, Transform::identity());
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(std::holds_alternative<RadialGradient>(*result));

  Shader shader = std::get<RadialGradient>(*result);
  EXPECT_TRUE(tiny_skia::isShaderOpaque(shader));

  tiny_skia::pipeline::RasterPipelineBuilder builder;
  EXPECT_TRUE(tiny_skia::pushShaderStages(shader, ColorSpace::Linear, builder));

  tiny_skia::transformShader(shader, Transform::fromTranslate(5.0f, 5.0f));
  tiny_skia::applyShaderOpacity(shader, 0.5f);
  EXPECT_FALSE(tiny_skia::isShaderOpaque(shader));
}

// ---------------------------------------------------------------------------
// Pattern tests
// ---------------------------------------------------------------------------

TEST(PatternTest, CreateAndPushStagesNearest) {
  // Create a small 4x4 pixmap for testing.
  std::vector<std::uint8_t> data(4 * 4 * 4, 255);  // 4x4 RGBA, all white
  const auto pixmap = tiny_skia::PixmapView::fromBytes(data, 4, 4);
  ASSERT_TRUE(pixmap.has_value());

  tiny_skia::Pattern pat(*pixmap, SpreadMode::Pad, tiny_skia::FilterQuality::Nearest, 1.0f,
                         Transform::identity());

  tiny_skia::pipeline::RasterPipelineBuilder builder;
  const bool ok = pat.pushStages(ColorSpace::Linear, builder);
  EXPECT_TRUE(ok);
  EXPECT_GE(builder.compile().stageCount(), 2u);
}

TEST(PatternTest, CreateAndPushStagesBilinear) {
  std::vector<std::uint8_t> data(4 * 4 * 4, 255);
  const auto pixmap = tiny_skia::PixmapView::fromBytes(data, 4, 4);
  ASSERT_TRUE(pixmap.has_value());

  tiny_skia::Pattern pat(*pixmap, SpreadMode::Pad, tiny_skia::FilterQuality::Bilinear, 1.0f,
                         Transform::fromScale(2.0f, 2.0f));

  tiny_skia::pipeline::RasterPipelineBuilder builder;
  const bool ok = pat.pushStages(ColorSpace::Linear, builder);
  EXPECT_TRUE(ok);
  EXPECT_GE(builder.compile().stageCount(), 3u);
}

TEST(PatternTest, CreateAndPushStagesBicubic) {
  std::vector<std::uint8_t> data(4 * 4 * 4, 255);
  const auto pixmap = tiny_skia::PixmapView::fromBytes(data, 4, 4);
  ASSERT_TRUE(pixmap.has_value());

  tiny_skia::Pattern pat(*pixmap, SpreadMode::Pad, tiny_skia::FilterQuality::Bicubic, 1.0f,
                         Transform::fromScale(2.0f, 2.0f));

  tiny_skia::pipeline::RasterPipelineBuilder builder;
  const bool ok = pat.pushStages(ColorSpace::Linear, builder);
  EXPECT_TRUE(ok);
  // Bicubic pushes Bicubic + Clamp0 + ClampA.
  EXPECT_GE(builder.compile().stageCount(), 4u);
}

TEST(PatternTest, NonInvertibleTransformFails) {
  std::vector<std::uint8_t> data(4 * 4 * 4, 255);
  const auto pixmap = tiny_skia::PixmapView::fromBytes(data, 4, 4);
  ASSERT_TRUE(pixmap.has_value());

  tiny_skia::Pattern pat(*pixmap, SpreadMode::Pad, tiny_skia::FilterQuality::Nearest, 1.0f,
                         Transform::fromScale(0.0f, 0.0f));

  tiny_skia::pipeline::RasterPipelineBuilder builder;
  EXPECT_FALSE(pat.pushStages(ColorSpace::Linear, builder));
}

TEST(PatternTest, PatternIsNeverOpaque) {
  std::vector<std::uint8_t> data(4 * 4 * 4, 255);
  const auto pixmap = tiny_skia::PixmapView::fromBytes(data, 4, 4);
  ASSERT_TRUE(pixmap.has_value());

  tiny_skia::Pattern pat(*pixmap, SpreadMode::Pad, tiny_skia::FilterQuality::Nearest, 1.0f,
                         Transform::identity());
  EXPECT_FALSE(pat.isOpaque());
}

TEST(PatternTest, OpacityAppliesScale1FloatStage) {
  std::vector<std::uint8_t> data(4 * 4 * 4, 255);
  const auto pixmap = tiny_skia::PixmapView::fromBytes(data, 4, 4);
  ASSERT_TRUE(pixmap.has_value());

  tiny_skia::Pattern pat(*pixmap, SpreadMode::Pad, tiny_skia::FilterQuality::Nearest, 0.5f,
                         Transform::identity());

  tiny_skia::pipeline::RasterPipelineBuilder builder;
  const bool ok = pat.pushStages(ColorSpace::Linear, builder);
  EXPECT_TRUE(ok);
  // With opacity < 1, adds Scale1Float.
  EXPECT_GE(builder.compile().stageCount(), 3u);
}

TEST(PatternTest, RepeatSpreadModePushesRepeatStage) {
  std::vector<std::uint8_t> data(4 * 4 * 4, 255);
  const auto pixmap = tiny_skia::PixmapView::fromBytes(data, 4, 4);
  ASSERT_TRUE(pixmap.has_value());

  tiny_skia::Pattern pat(*pixmap, SpreadMode::Repeat, tiny_skia::FilterQuality::Nearest, 1.0f,
                         Transform::identity());

  tiny_skia::pipeline::RasterPipelineBuilder builder;
  const bool ok = pat.pushStages(ColorSpace::Linear, builder);
  EXPECT_TRUE(ok);
  EXPECT_GE(builder.compile().stageCount(), 3u);
}

TEST(PatternTest, ShaderVariantDispatch) {
  std::vector<std::uint8_t> data(4 * 4 * 4, 255);
  const auto pixmap = tiny_skia::PixmapView::fromBytes(data, 4, 4);
  ASSERT_TRUE(pixmap.has_value());

  Shader shader = tiny_skia::Pattern(*pixmap, SpreadMode::Pad, tiny_skia::FilterQuality::Nearest,
                                     1.0f, Transform::identity());
  EXPECT_FALSE(tiny_skia::isShaderOpaque(shader));

  tiny_skia::pipeline::RasterPipelineBuilder builder;
  EXPECT_TRUE(tiny_skia::pushShaderStages(shader, ColorSpace::Linear, builder));

  tiny_skia::transformShader(shader, Transform::fromTranslate(5.0f, 5.0f));
  tiny_skia::applyShaderOpacity(shader, 0.5f);
}

}  // namespace
