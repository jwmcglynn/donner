#include "tiny_skia/Paint.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cmath>
#include <utility>
#include <vector>

#include "tiny_skia/Color.h"
#include "tiny_skia/Geom.h"
#include "tiny_skia/Pixmap.h"
#include "tiny_skia/Stroke.h"
#include "tiny_skia/Transform.h"
#include "tiny_skia/shaders/Shaders.h"

namespace {

using tiny_skia::Color;
using tiny_skia::FilterQuality;
using tiny_skia::GradientStop;
using tiny_skia::IntSize;
using tiny_skia::LinearGradient;
using tiny_skia::Paint;
using tiny_skia::Pattern;
using tiny_skia::Pixmap;
using tiny_skia::Point;
using tiny_skia::RadialGradient;
using tiny_skia::SpreadMode;
using tiny_skia::Stroke;
using tiny_skia::StrokeDash;
using tiny_skia::Transform;

std::vector<GradientStop> twoStops(Color first, Color second) {
  return {GradientStop::create(0.0f, first), GradientStop::create(1.0f, second)};
}

LinearGradient makeLinearGradient(std::vector<GradientStop> stops,
                                  Transform transform = Transform::identity()) {
  auto shader = LinearGradient::create(Point::fromXY(0.0f, 0.0f), Point::fromXY(8.0f, 8.0f),
                                       std::move(stops), SpreadMode::Pad, transform);
  EXPECT_TRUE(shader.has_value());
  EXPECT_TRUE(std::holds_alternative<LinearGradient>(*shader));
  return std::get<LinearGradient>(std::move(*shader));
}

RadialGradient makeRadialGradient(float radius) {
  auto shader = RadialGradient::create(Point::fromXY(4.0f, 4.0f), 0.0f, Point::fromXY(4.0f, 4.0f),
                                       radius, twoStops(Color::black, Color::white),
                                       SpreadMode::Pad, Transform::identity());
  EXPECT_TRUE(shader.has_value());
  EXPECT_TRUE(std::holds_alternative<RadialGradient>(*shader));
  return std::get<RadialGradient>(std::move(*shader));
}

Pixmap makePixmap(std::uint8_t fill) {
  const auto size = IntSize::fromWH(2, 2);
  EXPECT_TRUE(size.has_value());
  auto pixmap = Pixmap::fromVec(std::vector<std::uint8_t>(16, fill), *size);
  EXPECT_TRUE(pixmap.has_value());
  return std::move(*pixmap);
}

// A paint is only useful as an equality key if it is equal to itself, so every shader kind is
// checked for that before anything else.

TEST(PaintEquality, PaintIsEqualToItself) {
  Paint solid;
  solid.setColorRgba8(10, 20, 30, 40);
  EXPECT_EQ(solid, solid);

  Paint linear;
  linear.shader = makeLinearGradient(twoStops(Color::black, Color::white));
  EXPECT_EQ(linear, linear);

  Paint radial;
  radial.shader = makeRadialGradient(4.0f);
  EXPECT_EQ(radial, radial);

  const Pixmap pixmap = makePixmap(0x40);
  Paint pattern;
  pattern.shader = Pattern(pixmap.view(), SpreadMode::Repeat, FilterQuality::Nearest, 1.0f,
                           Transform::identity());
  EXPECT_EQ(pattern, pattern)
      << "a pattern paint that is not equal to itself would make every paint holding one "
         "unusable as a key, including in containers that assume reflexivity";
}

TEST(PaintEquality, SolidColorPaintsCompareByEveryField) {
  Paint base;
  base.setColorRgba8(10, 20, 30, 40);

  Paint same;
  same.setColorRgba8(10, 20, 30, 40);
  EXPECT_EQ(base, same);

  Paint otherColor;
  otherColor.setColorRgba8(10, 20, 31, 40);
  EXPECT_NE(base, otherColor);

  Paint otherAlias = same;
  otherAlias.antiAlias = !otherAlias.antiAlias;
  EXPECT_NE(base, otherAlias);

  Paint otherBlend = same;
  otherBlend.blendMode = tiny_skia::BlendMode::Multiply;
  EXPECT_NE(base, otherBlend);

  Paint otherColorspace = same;
  otherColorspace.colorspace = tiny_skia::ColorSpace::SimpleSRGB;
  EXPECT_NE(base, otherColorspace);

  Paint otherPipeline = same;
  otherPipeline.forceHqPipeline = !otherPipeline.forceHqPipeline;
  EXPECT_NE(base, otherPipeline);
}

TEST(PaintEquality, ShaderKindIsPartOfEquality) {
  Paint solid;
  solid.setColor(Color::black);

  Paint gradient;
  gradient.shader = makeLinearGradient(twoStops(Color::black, Color::black));

  EXPECT_NE(solid, gradient);
}

TEST(PaintEquality, GradientsCompareByStopsAndTransform) {
  Paint base;
  base.shader = makeLinearGradient(twoStops(Color::black, Color::white));

  Paint same;
  same.shader = makeLinearGradient(twoStops(Color::black, Color::white));
  EXPECT_EQ(base, same) << "two gradients built from the same inputs describe the same paint";

  Paint otherStopColor;
  otherStopColor.shader =
      makeLinearGradient(twoStops(Color::black, Color::fromRgba8(0, 255, 0, 255)));
  EXPECT_NE(base, otherStopColor);

  Paint otherStopPosition;
  otherStopPosition.shader = makeLinearGradient(
      {GradientStop::create(0.25f, Color::black), GradientStop::create(1.0f, Color::white)});
  EXPECT_NE(base, otherStopPosition);

  Paint otherStopCount;
  otherStopCount.shader = makeLinearGradient({GradientStop::create(0.0f, Color::black),
                                              GradientStop::create(0.5f, Color::black),
                                              GradientStop::create(1.0f, Color::white)});
  EXPECT_NE(base, otherStopCount);

  Paint otherTransform;
  otherTransform.shader = makeLinearGradient(twoStops(Color::black, Color::white),
                                             Transform::fromTranslate(1.0f, 0.0f));
  EXPECT_NE(base, otherTransform);
}

TEST(PaintEquality, RadialGradientsCompareByResolvedConfiguration) {
  Paint base;
  base.shader = makeRadialGradient(4.0f);

  Paint same;
  same.shader = makeRadialGradient(4.0f);
  EXPECT_EQ(base, same);

  Paint otherRadius;
  otherRadius.shader = makeRadialGradient(6.0f);
  EXPECT_NE(base, otherRadius);
}

TEST(PaintEquality, GradientStopsAreReportedForAccounting) {
  const LinearGradient gradient = makeLinearGradient(twoStops(Color::black, Color::white));
  EXPECT_EQ(gradient.base_.stops().size(), 2u);
  EXPECT_EQ(gradient.base_.stopsByteSize(), 2u * sizeof(GradientStop));
}

TEST(PaintEquality, PatternsCompareByEveryTilingField) {
  const Pixmap pixmap = makePixmap(0x40);
  const Pixmap otherPixmap = makePixmap(0x40);

  const auto makePattern = [&](const Pixmap& source, SpreadMode spread, FilterQuality quality,
                               float opacity, Transform transform) {
    Paint paint;
    paint.shader = Pattern(source.view(), spread, quality, opacity, transform);
    return paint;
  };

  const Paint base = makePattern(pixmap, SpreadMode::Repeat, FilterQuality::Nearest, 1.0f,
                                 Transform::identity());
  EXPECT_EQ(base, makePattern(pixmap, SpreadMode::Repeat, FilterQuality::Nearest, 1.0f,
                              Transform::identity()));

  EXPECT_NE(base, makePattern(pixmap, SpreadMode::Pad, FilterQuality::Nearest, 1.0f,
                              Transform::identity()));
  EXPECT_NE(base, makePattern(pixmap, SpreadMode::Repeat, FilterQuality::Bilinear, 1.0f,
                              Transform::identity()));
  EXPECT_NE(base, makePattern(pixmap, SpreadMode::Repeat, FilterQuality::Nearest, 0.5f,
                              Transform::identity()));
  EXPECT_NE(base, makePattern(pixmap, SpreadMode::Repeat, FilterQuality::Nearest, 1.0f,
                              Transform::fromTranslate(1.0f, 0.0f)));
  EXPECT_NE(base, makePattern(otherPixmap, SpreadMode::Repeat, FilterQuality::Nearest, 1.0f,
                              Transform::identity()))
      << "a pattern view names an address, so two buffers holding identical pixels are two "
         "different patterns";
}

// The float policy: IEEE comparison, whose two deviations from "same value" both fail toward
// reporting a difference rather than a false match.

TEST(PaintEquality, PaintHoldingNaNIsNeverEqual) {
  // Gradient construction rejects a non-finite transform, so the NaN is written afterwards:
  // the point is what equality does when one reaches a paint, not how one gets there.
  LinearGradient gradient = makeLinearGradient(twoStops(Color::black, Color::white));
  gradient.base_.transform = Transform::fromRow(std::nanf(""), 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);

  Paint paint;
  paint.shader = std::move(gradient);

  EXPECT_NE(paint, paint) << "IEEE comparison never reports a NaN as equal, so such a paint is "
                             "simply never reused, which is the safe direction";
}

TEST(PaintEquality, SignedZeroesCompareEqual) {
  Paint positiveZero;
  positiveZero.shader = makeLinearGradient(twoStops(Color::black, Color::white),
                                           Transform::fromTranslate(0.0f, 0.0f));

  Paint negativeZero;
  negativeZero.shader = makeLinearGradient(twoStops(Color::black, Color::white),
                                           Transform::fromTranslate(-0.0f, -0.0f));

  EXPECT_EQ(positiveZero, negativeZero)
      << "the two translate by the same amount, so treating them as one paint cannot change a "
         "pixel";
}

TEST(StrokeEquality, StrokesCompareByEveryField) {
  Stroke base;
  base.width = 2.0f;
  base.miterLimit = 4.0f;
  base.lineCap = tiny_skia::LineCap::Butt;
  base.lineJoin = tiny_skia::LineJoin::Miter;
  EXPECT_EQ(base, base);

  Stroke same = base;
  EXPECT_EQ(base, same);

  Stroke otherWidth = base;
  otherWidth.width = 2.5f;
  EXPECT_NE(base, otherWidth);

  Stroke otherMiter = base;
  otherMiter.miterLimit = 8.0f;
  EXPECT_NE(base, otherMiter);

  Stroke otherCap = base;
  otherCap.lineCap = tiny_skia::LineCap::Round;
  EXPECT_NE(base, otherCap);

  Stroke otherJoin = base;
  otherJoin.lineJoin = tiny_skia::LineJoin::Bevel;
  EXPECT_NE(base, otherJoin);
}

TEST(StrokeEquality, DashPatternsCompareByArrayAndOffset) {
  Stroke base;
  base.dash = StrokeDash::create({4.0f, 2.0f}, 0.0f);
  ASSERT_TRUE(base.dash.has_value());

  Stroke same;
  same.dash = StrokeDash::create({4.0f, 2.0f}, 0.0f);
  EXPECT_EQ(base, same);

  Stroke otherArray;
  otherArray.dash = StrokeDash::create({4.0f, 3.0f}, 0.0f);
  EXPECT_NE(base, otherArray);

  Stroke otherOffset;
  otherOffset.dash = StrokeDash::create({4.0f, 2.0f}, 1.0f);
  EXPECT_NE(base, otherOffset);

  Stroke undashed;
  EXPECT_NE(base, undashed);
}

TEST(StrokeEquality, StrokeHoldingNaNIsNeverEqual) {
  Stroke paint;
  paint.width = std::nanf("");
  EXPECT_NE(paint, paint);
}

}  // namespace
