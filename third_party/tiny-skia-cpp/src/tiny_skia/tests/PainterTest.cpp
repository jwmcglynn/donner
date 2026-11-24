#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>

#include "tiny_skia/Color.h"
#include "tiny_skia/Geom.h"
#include "tiny_skia/Mask.h"
#include "tiny_skia/Painter.h"
#include "tiny_skia/Path.h"
#include "tiny_skia/PathBuilder.h"
#include "tiny_skia/Pixmap.h"
#include "tiny_skia/pipeline/Pipeline.h"
#include "tiny_skia/shaders/Shaders.h"

namespace {

using tiny_skia::BlendMode;
using tiny_skia::Color;
using tiny_skia::ColorSpace;
using tiny_skia::detail::DrawTiler;
using tiny_skia::FillRule;
using tiny_skia::LineCap;
using tiny_skia::Mask;
using tiny_skia::Paint;
using tiny_skia::Path;
using tiny_skia::PathBuilder;
using tiny_skia::PathVerb;
using tiny_skia::Pixmap;
using tiny_skia::MutablePixmapView;
using tiny_skia::PixmapPaint;
using tiny_skia::PixmapView;
using tiny_skia::Point;
using tiny_skia::Rect;
using tiny_skia::ScreenIntRect;
using tiny_skia::Stroke;
using tiny_skia::Transform;

// ---- Paint tests ----

TEST(PaintTest, DefaultPaint) {
  Paint paint;
  EXPECT_TRUE(paint.isSolidColor());
  EXPECT_EQ(paint.blendMode, BlendMode::SourceOver);
  EXPECT_TRUE(paint.antiAlias);
  EXPECT_EQ(paint.colorspace, ColorSpace::Linear);
  EXPECT_FALSE(paint.forceHqPipeline);
}

TEST(PaintTest, SetColor) {
  Paint paint;
  paint.setColor(Color::white);
  EXPECT_TRUE(paint.isSolidColor());
  const auto& c = std::get<Color>(paint.shader);
  EXPECT_FLOAT_EQ(c.red(), 1.0f);
  EXPECT_FLOAT_EQ(c.green(), 1.0f);
  EXPECT_FLOAT_EQ(c.blue(), 1.0f);
  EXPECT_FLOAT_EQ(c.alpha(), 1.0f);
}

TEST(PaintTest, SetColorRgba8) {
  Paint paint;
  paint.setColorRgba8(128, 64, 32, 255);
  EXPECT_TRUE(paint.isSolidColor());
}

TEST(PaintTest, IsSolidColorFalseForGradient) {
  auto stops = std::vector<tiny_skia::GradientStop>{
      tiny_skia::GradientStop::create(0.0f, Color::black),
      tiny_skia::GradientStop::create(1.0f, Color::white),
  };
  auto result = tiny_skia::LinearGradient::create(Point::fromXY(0, 0), Point::fromXY(100, 0),
                                                  std::move(stops), tiny_skia::SpreadMode::Pad,
                                                  Transform::identity());
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(std::holds_alternative<tiny_skia::LinearGradient>(*result));

  Paint paint;
  paint.shader = std::get<tiny_skia::LinearGradient>(std::move(*result));
  EXPECT_FALSE(paint.isSolidColor());
}

// ---- DrawTiler tests ----

TEST(DrawTilerTest, SmallPixmapDoesNotRequireTiling) {
  EXPECT_FALSE(DrawTiler::required(100, 500));
  EXPECT_FALSE(DrawTiler::create(100, 500).has_value());
}

TEST(DrawTilerTest, LargePixmapRequiresTiling) {
  EXPECT_TRUE(DrawTiler::required(10000, 500));
  EXPECT_TRUE(DrawTiler::create(10000, 500).has_value());
}

TEST(DrawTilerTest, HorizontalTiling) {
  constexpr auto kMax = DrawTiler::kMaxDimensions;
  auto tiler = DrawTiler::create(10000, 500);
  ASSERT_TRUE(tiler.has_value());

  auto t1 = tiler->next();
  ASSERT_TRUE(t1.has_value());
  EXPECT_EQ(t1->x(), 0u);
  EXPECT_EQ(t1->y(), 0u);
  EXPECT_EQ(t1->width(), kMax);
  EXPECT_EQ(t1->height(), 500u);

  auto t2 = tiler->next();
  ASSERT_TRUE(t2.has_value());
  EXPECT_EQ(t2->x(), kMax);
  EXPECT_EQ(t2->y(), 0u);
  EXPECT_EQ(t2->width(), 10000 - kMax);
  EXPECT_EQ(t2->height(), 500u);

  EXPECT_FALSE(tiler->next().has_value());
}

TEST(DrawTilerTest, VerticalTiling) {
  constexpr auto kMax = DrawTiler::kMaxDimensions;
  auto tiler = DrawTiler::create(500, 10000);
  ASSERT_TRUE(tiler.has_value());

  auto t1 = tiler->next();
  ASSERT_TRUE(t1.has_value());
  EXPECT_EQ(t1->x(), 0u);
  EXPECT_EQ(t1->y(), 0u);
  EXPECT_EQ(t1->width(), 500u);
  EXPECT_EQ(t1->height(), kMax);

  auto t2 = tiler->next();
  ASSERT_TRUE(t2.has_value());
  EXPECT_EQ(t2->x(), 0u);
  EXPECT_EQ(t2->y(), kMax);
  EXPECT_EQ(t2->width(), 500u);
  EXPECT_EQ(t2->height(), 10000 - kMax);

  EXPECT_FALSE(tiler->next().has_value());
}

TEST(DrawTilerTest, RectTiling) {
  constexpr auto kMax = DrawTiler::kMaxDimensions;
  auto tiler = DrawTiler::create(10000, 10000);
  ASSERT_TRUE(tiler.has_value());

  // Row 1.
  auto t1 = tiler->next();
  ASSERT_TRUE(t1.has_value());
  EXPECT_EQ(t1->x(), 0u);
  EXPECT_EQ(t1->y(), 0u);
  EXPECT_EQ(t1->width(), kMax);
  EXPECT_EQ(t1->height(), kMax);

  auto t2 = tiler->next();
  ASSERT_TRUE(t2.has_value());
  EXPECT_EQ(t2->x(), kMax);
  EXPECT_EQ(t2->y(), 0u);
  EXPECT_EQ(t2->width(), 10000 - kMax);
  EXPECT_EQ(t2->height(), kMax);

  // Row 2.
  auto t3 = tiler->next();
  ASSERT_TRUE(t3.has_value());
  EXPECT_EQ(t3->x(), 0u);
  EXPECT_EQ(t3->y(), kMax);
  EXPECT_EQ(t3->width(), kMax);
  EXPECT_EQ(t3->height(), 10000 - kMax);

  auto t4 = tiler->next();
  ASSERT_TRUE(t4.has_value());
  EXPECT_EQ(t4->x(), kMax);
  EXPECT_EQ(t4->y(), kMax);
  EXPECT_EQ(t4->width(), 10000 - kMax);
  EXPECT_EQ(t4->height(), 10000 - kMax);

  EXPECT_FALSE(tiler->next().has_value());
}

// ---- isTooBigForMath tests ----

TEST(PainterHelpersTest, IsTooBigForMathSmallPath) {
  auto path = tiny_skia::Path::fromRect(*Rect::fromLTRB(0.0f, 0.0f, 100.0f, 100.0f));
  EXPECT_FALSE(tiny_skia::detail::isTooBigForMath(path));
}

TEST(PainterHelpersTest, IsTooBigForMathHugePath) {
  const float big = std::numeric_limits<float>::max() * 0.5f;
  auto path = tiny_skia::Path::fromRect(*Rect::fromLTRB(-big, -big, big, big));
  EXPECT_TRUE(tiny_skia::detail::isTooBigForMath(path));
}

// ---- treatAsHairline tests ----

TEST(PainterHelpersTest, TreatAsHairlineZeroWidth) {
  Paint paint;
  auto result = tiny_skia::detail::treatAsHairline(paint, 0.0f, Transform::identity());
  ASSERT_TRUE(result.has_value());
  EXPECT_FLOAT_EQ(*result, 1.0f);
}

TEST(PainterHelpersTest, TreatAsHairlineNotAntiAliased) {
  Paint paint;
  paint.antiAlias = false;
  auto result = tiny_skia::detail::treatAsHairline(paint, 0.5f, Transform::identity());
  EXPECT_FALSE(result.has_value());
}

TEST(PainterHelpersTest, TreatAsHairlineThinStroke) {
  Paint paint;
  paint.antiAlias = true;
  auto result = tiny_skia::detail::treatAsHairline(paint, 0.5f, Transform::identity());
  ASSERT_TRUE(result.has_value());
  // fastLen(0.5, 0) = 0.5, fastLen(0, 0.5) = 0.5, ave = 0.5
  EXPECT_FLOAT_EQ(*result, 0.5f);
}

TEST(PainterHelpersTest, TreatAsHairlineThickStroke) {
  Paint paint;
  paint.antiAlias = true;
  auto result = tiny_skia::detail::treatAsHairline(paint, 5.0f, Transform::identity());
  EXPECT_FALSE(result.has_value());
}

// ---- Transform::mapPoints tests ----

TEST(TransformTest, MapPointsIdentity) {
  Point pts[2] = {Point::fromXY(10.0f, 20.0f), Point::fromXY(30.0f, 40.0f)};
  Transform::identity().mapPoints(pts);
  EXPECT_FLOAT_EQ(pts[0].x, 10.0f);
  EXPECT_FLOAT_EQ(pts[0].y, 20.0f);
  EXPECT_FLOAT_EQ(pts[1].x, 30.0f);
  EXPECT_FLOAT_EQ(pts[1].y, 40.0f);
}

TEST(TransformTest, MapPointsTranslate) {
  Point pts[2] = {Point::fromXY(0.0f, 0.0f), Point::fromXY(1.0f, 1.0f)};
  Transform::fromTranslate(10.0f, 20.0f).mapPoints(pts);
  EXPECT_FLOAT_EQ(pts[0].x, 10.0f);
  EXPECT_FLOAT_EQ(pts[0].y, 20.0f);
  EXPECT_FLOAT_EQ(pts[1].x, 11.0f);
  EXPECT_FLOAT_EQ(pts[1].y, 21.0f);
}

TEST(TransformTest, MapPointsScale) {
  Point pts[1] = {Point::fromXY(3.0f, 4.0f)};
  Transform::fromScale(2.0f, 3.0f).mapPoints(pts);
  EXPECT_FLOAT_EQ(pts[0].x, 6.0f);
  EXPECT_FLOAT_EQ(pts[0].y, 12.0f);
}

// ---- Path::fromRect tests ----

TEST(PathHelperTest, PathFromRect) {
  auto rect = Rect::fromLTRB(10.0f, 20.0f, 30.0f, 40.0f);
  ASSERT_TRUE(rect.has_value());
  auto path = tiny_skia::Path::fromRect(*rect);
  EXPECT_EQ(path.verbs().size(), 5u);
  EXPECT_EQ(path.points().size(), 4u);

  auto bounds = path.bounds();
  EXPECT_FLOAT_EQ(bounds.left(), 10.0f);
  EXPECT_FLOAT_EQ(bounds.top(), 20.0f);
  EXPECT_FLOAT_EQ(bounds.right(), 30.0f);
  EXPECT_FLOAT_EQ(bounds.bottom(), 40.0f);
}

// ---- Path::transform tests ----

TEST(PathTest, TransformIdentityNoChange) {
  auto path = tiny_skia::Path::fromRect(*Rect::fromLTRB(0.0f, 0.0f, 10.0f, 10.0f));
  auto result = path.transform(Transform::identity());
  ASSERT_TRUE(result.has_value());
  EXPECT_FLOAT_EQ(result->bounds().left(), 0.0f);
  EXPECT_FLOAT_EQ(result->bounds().right(), 10.0f);
}

TEST(PathTest, TransformTranslate) {
  auto path = tiny_skia::Path::fromRect(*Rect::fromLTRB(0.0f, 0.0f, 10.0f, 10.0f));
  auto result = path.transform(Transform::fromTranslate(5.0f, 10.0f));
  ASSERT_TRUE(result.has_value());
  EXPECT_FLOAT_EQ(result->bounds().left(), 5.0f);
  EXPECT_FLOAT_EQ(result->bounds().top(), 10.0f);
  EXPECT_FLOAT_EQ(result->bounds().right(), 15.0f);
  EXPECT_FLOAT_EQ(result->bounds().bottom(), 20.0f);
}

// ---- Rect::width/height tests ----

TEST(RectTest, WidthHeight) {
  auto r = Rect::fromLTRB(10.0f, 20.0f, 50.0f, 60.0f);
  ASSERT_TRUE(r.has_value());
  EXPECT_FLOAT_EQ(r->width(), 40.0f);
  EXPECT_FLOAT_EQ(r->height(), 40.0f);
}

// ---- IntSize::toIntRect/toRect tests ----

TEST(IntSizeTest, ToIntRect) {
  auto s = tiny_skia::IntSize::fromWH(100, 200);
  ASSERT_TRUE(s.has_value());
  auto r = s->toIntRect(5, 10);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->x(), 5);
  EXPECT_EQ(r->y(), 10);
  EXPECT_EQ(r->width(), 100u);
  EXPECT_EQ(r->height(), 200u);
}

TEST(IntSizeTest, ToRect) {
  auto s = tiny_skia::IntSize::fromWH(100, 200);
  ASSERT_TRUE(s.has_value());
  auto r = s->toRect();
  EXPECT_FLOAT_EQ(r.left(), 0.0f);
  EXPECT_FLOAT_EQ(r.top(), 0.0f);
  EXPECT_FLOAT_EQ(r.right(), 100.0f);
  EXPECT_FLOAT_EQ(r.bottom(), 200.0f);
}

// ---- fillRect integration tests ----

TEST(FillRectTest, SolidColorFill) {
  auto pixmap = Pixmap::fromSize(10, 10);
  ASSERT_TRUE(pixmap.has_value());
  pixmap->fill(Color::fromRgba8(0, 0, 0, 255));

  auto mut = pixmap->mutableView();
  auto rect = Rect::fromLTRB(2.0f, 2.0f, 8.0f, 8.0f);
  ASSERT_TRUE(rect.has_value());

  Paint paint;
  paint.setColor(Color::fromRgba8(255, 0, 0, 255));
  paint.antiAlias = false;

  tiny_skia::Painter::fillRect(mut, *rect, paint, Transform::identity());

  // Check center pixel is red.
  auto pixel = pixmap->pixel(5, 5);
  ASSERT_TRUE(pixel.has_value());
  EXPECT_EQ(pixel->red(), 255u);
  EXPECT_EQ(pixel->green(), 0u);
  EXPECT_EQ(pixel->blue(), 0u);
  EXPECT_EQ(pixel->alpha(), 255u);

  // Check corner pixel (outside rect) is black.
  auto corner = pixmap->pixel(0, 0);
  ASSERT_TRUE(corner.has_value());
  EXPECT_EQ(corner->red(), 0u);
  EXPECT_EQ(corner->green(), 0u);
  EXPECT_EQ(corner->blue(), 0u);
}

TEST(FillRectTest, WithTransformDelegatesToFillPath) {
  auto pixmap = Pixmap::fromSize(10, 10);
  ASSERT_TRUE(pixmap.has_value());
  pixmap->fill(Color::fromRgba8(0, 0, 0, 255));

  auto mut = pixmap->mutableView();
  auto rect = Rect::fromLTRB(0.0f, 0.0f, 5.0f, 5.0f);
  ASSERT_TRUE(rect.has_value());

  Paint paint;
  paint.setColor(Color::fromRgba8(0, 255, 0, 255));
  paint.antiAlias = false;

  // Translate by (3, 3) - should put the rectangle at (3,3)-(8,8).
  tiny_skia::Painter::fillRect(mut, *rect, paint, Transform::fromTranslate(3.0f, 3.0f));

  // Pixel at (5,5) should be green.
  auto pixel = pixmap->pixel(5, 5);
  ASSERT_TRUE(pixel.has_value());
  EXPECT_EQ(pixel->red(), 0u);
  EXPECT_EQ(pixel->green(), 255u);

  // Pixel at (1,1) should still be black.
  auto corner = pixmap->pixel(1, 1);
  ASSERT_TRUE(corner.has_value());
  EXPECT_EQ(corner->green(), 0u);
}

// ---- fillPath integration tests ----

TEST(FillPathTest, SimpleRectanglePath) {
  auto pixmap = Pixmap::fromSize(10, 10);
  ASSERT_TRUE(pixmap.has_value());
  pixmap->fill(Color::fromRgba8(0, 0, 0, 255));

  auto mut = pixmap->mutableView();
  auto path = tiny_skia::Path::fromRect(*Rect::fromLTRB(1.0f, 1.0f, 9.0f, 9.0f));

  Paint paint;
  paint.setColor(Color::fromRgba8(0, 0, 255, 255));
  paint.antiAlias = false;

  tiny_skia::Painter::fillPath(mut, path, paint, FillRule::Winding, Transform::identity());

  // Center should be blue.
  auto pixel = pixmap->pixel(5, 5);
  ASSERT_TRUE(pixel.has_value());
  EXPECT_EQ(pixel->blue(), 255u);
  EXPECT_EQ(pixel->red(), 0u);
}

TEST(FillPathTest, EmptyPathIsNoOp) {
  auto pixmap = Pixmap::fromSize(10, 10);
  ASSERT_TRUE(pixmap.has_value());
  pixmap->fill(Color::fromRgba8(128, 128, 128, 255));

  auto mut = pixmap->mutableView();
  // Empty line (zero height).
  auto path = tiny_skia::Path::fromRect(*Rect::fromLTRB(0.0f, 5.0f, 10.0f, 5.0f));

  Paint paint;
  paint.setColor(Color::fromRgba8(255, 0, 0, 255));
  paint.antiAlias = false;

  tiny_skia::Painter::fillPath(mut, path, paint, FillRule::Winding, Transform::identity());

  // Should still be gray.
  auto pixel = pixmap->pixel(5, 5);
  ASSERT_TRUE(pixel.has_value());
  EXPECT_EQ(pixel->red(), 128u);
}

TEST(FillPathTest, WithTransform) {
  auto pixmap = Pixmap::fromSize(20, 20);
  ASSERT_TRUE(pixmap.has_value());
  pixmap->fill(Color::fromRgba8(0, 0, 0, 255));

  auto mut = pixmap->mutableView();
  auto path = tiny_skia::Path::fromRect(*Rect::fromLTRB(0.0f, 0.0f, 5.0f, 5.0f));

  Paint paint;
  paint.setColor(Color::fromRgba8(255, 255, 0, 255));
  paint.antiAlias = false;

  // Scale 2x.
  tiny_skia::Painter::fillPath(mut, path, paint, FillRule::Winding, Transform::fromScale(2.0f, 2.0f));

  // Pixel at (5,5) should be yellow (inside 2x-scaled rect: 0-10).
  auto pixel = pixmap->pixel(5, 5);
  ASSERT_TRUE(pixel.has_value());
  EXPECT_EQ(pixel->red(), 255u);
  EXPECT_EQ(pixel->green(), 255u);

  // Pixel at (15,15) should still be black (outside).
  auto outer = pixmap->pixel(15, 15);
  ASSERT_TRUE(outer.has_value());
  EXPECT_EQ(outer->red(), 0u);
}

// ---- drawPixmap integration test ----

TEST(DrawPixmapTest, DrawOntoPixmapDoesNotCrash) {
  // Smoke test: drawPixmap should run without crashing.
  // Note: Correct pixel values require the Gather pipeline stage to be
  // implemented (currently a stub in Highp.cpp/Lowp.cpp). This test verifies
  // the orchestration code is wired correctly.
  auto src = Pixmap::fromSize(4, 4);
  ASSERT_TRUE(src.has_value());
  src->fill(Color::fromRgba8(255, 0, 0, 255));

  auto dst = Pixmap::fromSize(10, 10);
  ASSERT_TRUE(dst.has_value());
  dst->fill(Color::fromRgba8(0, 0, 0, 255));

  auto mut = dst->mutableView();
  PixmapPaint ppaint;
  ppaint.opacity = 1.0f;
  ppaint.blendMode = BlendMode::Source;

  // Should not crash.
  tiny_skia::Painter::drawPixmap(mut, 3, 3, src->view(), ppaint, Transform::identity());

  // Pixel outside the drawn area should be unchanged.
  auto corner = dst->pixel(0, 0);
  ASSERT_TRUE(corner.has_value());
  EXPECT_EQ(corner->red(), 0u);
  EXPECT_EQ(corner->alpha(), 255u);
}

// ---- applyMask test ----

TEST(ApplyMaskTest, MaskMasksOutContent) {
  auto pixmap = Pixmap::fromSize(4, 4);
  ASSERT_TRUE(pixmap.has_value());
  pixmap->fill(Color::fromRgba8(255, 0, 0, 255));

  // Create a mask that is all-zero (transparent).
  auto mask = Mask::fromSize(4, 4);
  ASSERT_TRUE(mask.has_value());

  auto mut = pixmap->mutableView();
  tiny_skia::Painter::applyMask(mut, *mask);

  // All pixels should be transparent (masked out).
  auto pixel = pixmap->pixel(2, 2);
  ASSERT_TRUE(pixel.has_value());
  EXPECT_EQ(pixel->alpha(), 0u);
}

TEST(ApplyMaskTest, MismatchedSizeIsNoOp) {
  auto pixmap = Pixmap::fromSize(4, 4);
  ASSERT_TRUE(pixmap.has_value());
  pixmap->fill(Color::fromRgba8(255, 0, 0, 255));

  auto mask = Mask::fromSize(8, 8);
  ASSERT_TRUE(mask.has_value());

  auto mut = pixmap->mutableView();
  tiny_skia::Painter::applyMask(mut, *mask);

  // Should be unchanged - still red.
  auto pixel = pixmap->pixel(2, 2);
  ASSERT_TRUE(pixel.has_value());
  EXPECT_EQ(pixel->red(), 255u);
  EXPECT_EQ(pixel->alpha(), 255u);
}

TEST(StrokePathTest, AxisAlignedMiterRectangleUsesExactRingCoverage) {
  auto pixmap = Pixmap::fromSize(32, 32);
  ASSERT_TRUE(pixmap);
  auto dst = pixmap->mutableView();
  const auto rect = Rect::fromXYWH(1.0f, 1.0f, 18.0f, 10.0f);
  ASSERT_TRUE(rect);
  const auto path = Path::fromRect(*rect);
  Paint paint;
  paint.setColorRgba8(255, 0, 0, 255);

  // In device space the outer top and left edges are at 0.625. The inner top is at 1.875.
  tiny_skia::Painter::strokePath(dst, path, paint, Stroke{}, Transform::fromScale(1.25f, 1.25f));
  EXPECT_NEAR(pixmap->pixel(8, 0)->alpha(), 96, 1);
  EXPECT_NEAR(pixmap->pixel(8, 1)->alpha(), 223, 1);
  EXPECT_NEAR(pixmap->pixel(0, 0)->alpha(), 36, 1);
  EXPECT_EQ(pixmap->pixel(8, 3)->alpha(), 0u);
}

TEST(StrokePathTest, AxisAlignedRectangleAppliesClipAndBlendOnce) {
  auto pixmap = Pixmap::fromSize(32, 32);
  auto mask = Mask::fromSize(32, 32);
  ASSERT_TRUE(pixmap);
  ASSERT_TRUE(mask);
  auto dst = pixmap->mutableView();
  std::fill(mask->data().begin(), mask->data().end(), 128u);
  mask->data()[32 + 8] = 0;
  const auto rect = Rect::fromXYWH(1.0f, 1.0f, 18.0f, 10.0f);
  ASSERT_TRUE(rect);
  const auto path = Path::fromRect(*rect);
  Paint paint;
  paint.setColorRgba8(255, 0, 0, 128);
  paint.blendMode = BlendMode::Source;

  tiny_skia::Painter::strokePath(dst, path, paint, Stroke{}, Transform::fromScale(1.25f, 1.25f),
                                 &*mask);
  EXPECT_NEAR(pixmap->pixel(8, 0)->alpha(), 24, 1);
  EXPECT_NEAR(pixmap->pixel(0, 0)->alpha(), 9, 1);
  EXPECT_EQ(pixmap->pixel(8, 1)->alpha(), 0u);
  EXPECT_EQ(pixmap->pixel(8, 3)->alpha(), 0u);
}

TEST(StrokePathTest, AxisAlignedRectangleRecognizesReverseTraversalAndClipsOffscreen) {
  auto pixmap = Pixmap::fromSize(32, 32);
  ASSERT_TRUE(pixmap);
  PathBuilder builder;
  builder.moveTo(19.0f, 11.0f);
  builder.lineTo(19.0f, 1.0f);
  builder.lineTo(1.0f, 1.0f);
  builder.lineTo(1.0f, 11.0f);
  builder.close();
  const auto rect = builder.finish();
  ASSERT_TRUE(rect);

  Paint paint;
  paint.setColorRgba8(255, 0, 0, 255);
  const auto transform = Transform::fromRow(1.25f, 0.0f, 0.0f, 1.25f, -1.0f, -1.0f);
  auto dst = pixmap->mutableView();
  tiny_skia::Painter::strokePath(dst, *rect, paint, Stroke{}, transform);
  // The clipped pixel at the corner contains the full outer rectangle and 1/64 of its hole.
  EXPECT_NEAR(pixmap->pixel(0, 0)->alpha(), 251, 1);
  EXPECT_NEAR(pixmap->pixel(8, 0)->alpha(), 223, 1);
  EXPECT_EQ(pixmap->pixel(8, 2)->alpha(), 0u);
}

TEST(StrokePathTest, AxisAlignedRectangleCompositesOverAnExistingDestination) {
  auto pixmap = Pixmap::fromSize(32, 32);
  ASSERT_TRUE(pixmap);
  pixmap->fill(Color::fromRgba8(0, 0, 255, 255));
  const auto rect = Rect::fromXYWH(1.0f, 1.0f, 18.0f, 10.0f);
  ASSERT_TRUE(rect);
  const auto path = Path::fromRect(*rect);
  const auto transform = Transform::fromScale(1.25f, 1.25f);
  auto dst = pixmap->mutableView();
  Paint paint;
  paint.setColorRgba8(255, 0, 0, 128);

  tiny_skia::Painter::strokePath(dst, path, paint, Stroke{}, transform);
  const auto blended = pixmap->pixel(8, 0);
  ASSERT_TRUE(blended);
  EXPECT_NEAR(blended->red(), 48, 1);
  EXPECT_NEAR(blended->blue(), 207, 1);
  EXPECT_EQ(blended->alpha(), 255u);

  paint.setColorRgba8(0, 0, 0, 0);
  paint.blendMode = BlendMode::Source;
  tiny_skia::Painter::strokePath(dst, path, paint, Stroke{}, transform);
  EXPECT_NEAR(pixmap->pixel(8, 0)->alpha(), 159, 1);
  EXPECT_EQ(pixmap->pixel(8, 3)->alpha(), 255u);
}

TEST(StrokePathTest, AxisAlignedRectangleSupportsSubpixelWidthOnOneAxis) {
  auto pixmap = Pixmap::fromSize(64, 24);
  ASSERT_TRUE(pixmap);
  const auto rect = Rect::fromXYWH(1.0f, 4.5f, 2.0f, 12.0f);
  ASSERT_TRUE(rect);
  Paint paint;
  paint.setColorRgba8(255, 0, 0, 255);
  Stroke stroke;
  stroke.width = 0.125f;
  auto dst = pixmap->mutableView();

  // The other axis has a two-pixel stroke, so the existing hairline rule does not apply.
  tiny_skia::Painter::strokePath(dst, Path::fromRect(*rect), paint, stroke,
                                 Transform::fromScale(16.0f, 1.0f));
  EXPECT_NEAR(pixmap->pixel(26, 4)->alpha(), 32, 1);
  EXPECT_EQ(pixmap->pixel(15, 8)->alpha(), 255u);
  EXPECT_EQ(pixmap->pixel(26, 8)->alpha(), 0u);
}

TEST(StrokePathTest, AxisAlignedRectanglePreservesHairlineAndReflectedFallbacks) {
  auto pixmap = Pixmap::fromSize(32, 32);
  ASSERT_TRUE(pixmap);
  const auto rect = Rect::fromXYWH(1.0f, 1.0f, 18.0f, 10.0f);
  ASSERT_TRUE(rect);
  Paint paint;
  paint.setColorRgba8(255, 0, 0, 255);
  const auto path = Path::fromRect(*rect);
  auto dst = pixmap->mutableView();
  const auto reflected = Transform::fromRow(-1.25f, 0.0f, 0.0f, 1.25f, 25.0f, 0.0f);
  tiny_skia::Painter::strokePath(dst, path, paint, Stroke{}, reflected);
  EXPECT_EQ(pixmap->pixel(8, 0)->alpha(), 64u);
  EXPECT_EQ(pixmap->pixel(8, 1)->alpha(), 255u);

  auto hairline = Pixmap::fromSize(32, 32);
  ASSERT_TRUE(hairline);
  auto hairlineDst = hairline->mutableView();
  Stroke stroke;
  stroke.width = 0.125f;
  tiny_skia::Painter::strokePath(hairlineDst, path, paint, stroke);
  EXPECT_EQ(hairline->pixel(8, 3)->alpha(), 0u);
  EXPECT_LT(hairline->pixel(8, 0)->alpha(), 32u);
  EXPECT_GT(hairline->pixel(8, 1)->alpha(), 0u);
}

TEST(StrokePathTest, AxisAlignedRectangleHonorsBudgetAndSkipsOffscreenRing) {
  auto pixmap = Pixmap::fromSize(32, 32);
  ASSERT_TRUE(pixmap);
  const auto huge = Rect::fromXYWH(-1000000.0f, -1000000.0f, 2000000.0f, 2000000.0f);
  const auto visible = Rect::fromXYWH(1.0f, 1.0f, 18.0f, 10.0f);
  ASSERT_TRUE(huge);
  ASSERT_TRUE(visible);
  Paint paint;
  paint.setColorRgba8(255, 0, 0, 255);
  Stroke stroke;
  stroke.width = 2.0f;
  auto dst = pixmap->mutableView();
  tiny_skia::Painter::strokePath(dst, Path::fromRect(*huge), paint, stroke);
  tiny_skia::StrokeLimits limit{.maxOutlinePoints = 1};
  tiny_skia::Painter::strokePath(dst, Path::fromRect(*visible), paint, stroke,
                                 Transform::identity(), nullptr, nullptr, &limit);
  EXPECT_TRUE(limit.exceeded);
  EXPECT_THAT(pixmap->data(), testing::Each(0u));
}

// ---- strokeHairline test ----

TEST(StrokeHairlineTest, BasicStroke) {
  auto pixmap = Pixmap::fromSize(10, 10);
  ASSERT_TRUE(pixmap.has_value());
  pixmap->fill(Color::fromRgba8(0, 0, 0, 255));

  // A simple horizontal line path.
  PathBuilder builder;
  builder.moveTo(1.0f, 5.0f);
  builder.lineTo(9.0f, 5.0f);
  auto path = builder.finish();
  ASSERT_TRUE(path.has_value());

  Paint paint;
  paint.setColor(Color::fromRgba8(255, 0, 0, 255));
  paint.antiAlias = false;

  Stroke stroke;
  stroke.width = 1.0f;
  stroke.lineCap = LineCap::Butt;

  auto mut = pixmap->mutableView();
  tiny_skia::Painter::strokePath(mut, *path, paint, stroke);

  // The hairline should have drawn on row 5.
  auto pixel = pixmap->pixel(5, 5);
  ASSERT_TRUE(pixel.has_value());
  EXPECT_EQ(pixel->red(), 255u);
}

// ---- Blend mode tests ----

TEST(FillRectTest, DestinationBlendModeIsNoOp) {
  auto pixmap = Pixmap::fromSize(4, 4);
  ASSERT_TRUE(pixmap.has_value());
  pixmap->fill(Color::fromRgba8(100, 100, 100, 255));

  auto mut = pixmap->mutableView();
  auto rect = Rect::fromLTRB(0.0f, 0.0f, 4.0f, 4.0f);
  ASSERT_TRUE(rect.has_value());

  Paint paint;
  paint.setColor(Color::fromRgba8(255, 0, 0, 255));
  paint.blendMode = BlendMode::Destination;
  paint.antiAlias = false;

  tiny_skia::Painter::fillRect(mut, *rect, paint, Transform::identity());

  // Should be unchanged (Destination blend is a no-op).
  auto pixel = pixmap->pixel(2, 2);
  ASSERT_TRUE(pixel.has_value());
  EXPECT_EQ(pixel->red(), 100u);
}

TEST(FillRectTest, ClearBlendMode) {
  auto pixmap = Pixmap::fromSize(4, 4);
  ASSERT_TRUE(pixmap.has_value());
  pixmap->fill(Color::fromRgba8(255, 0, 0, 255));

  auto mut = pixmap->mutableView();
  auto rect = Rect::fromLTRB(0.0f, 0.0f, 4.0f, 4.0f);
  ASSERT_TRUE(rect.has_value());

  Paint paint;
  paint.setColor(Color::fromRgba8(0, 0, 0, 0));
  paint.blendMode = BlendMode::Clear;
  paint.antiAlias = false;

  tiny_skia::Painter::fillRect(mut, *rect, paint, Transform::identity());

  // Should be transparent.
  auto pixel = pixmap->pixel(2, 2);
  ASSERT_TRUE(pixel.has_value());
  EXPECT_EQ(pixel->alpha(), 0u);
}

}  // namespace

// ---- Empty destination tests (Donner addition) ----

// A surface that could not be allocated is an empty pixmap. Rectangle, path, stroke and hairline
// fills and pixmap draws into it must do nothing: its size has no valid clip rectangle to build.
TEST(PainterEmptyDestinationTest, DrawsIntoAnEmptyPixmapDoNothing) {
  Pixmap empty;
  ASSERT_TRUE(empty.size().isEmpty());
  auto dst = empty.mutableView();
  Paint paint;
  paint.setColor(Color::fromRgba8(255, 0, 0, 255));
  const auto rect = Rect::fromLTRB(0.0f, 0.0f, 8.0f, 8.0f);
  ASSERT_TRUE(rect.has_value());
  const auto path = tiny_skia::Path::fromRect(*rect);

  for (const bool antiAlias : {false, true}) {
    paint.antiAlias = antiAlias;
    tiny_skia::Painter::fillRect(dst, *rect, paint, Transform::identity());
    tiny_skia::Painter::fillPath(dst, path, paint, FillRule::Winding, Transform::identity());
    tiny_skia::Painter::strokePath(dst, path, paint, Stroke{}, Transform::identity());
    Stroke wide;
    wide.width = 3.0f;
    tiny_skia::Painter::strokePath(dst, path, paint, wide, Transform::identity());
  }

  auto src = Pixmap::fromSize(4, 4);
  ASSERT_TRUE(src.has_value());
  src->fill(Color::fromRgba8(0, 0, 255, 255));
  PixmapPaint ppaint;
  tiny_skia::Painter::drawPixmap(dst, 0, 0, src->view(), ppaint, Transform::identity());
  tiny_skia::Painter::drawPixmap(dst, 1, 1, src->view(), ppaint,
                                 Transform::fromScale(2.0f, 2.0f));
  EXPECT_TRUE(empty.size().isEmpty());
}
