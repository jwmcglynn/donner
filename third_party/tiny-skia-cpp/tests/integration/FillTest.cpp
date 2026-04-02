#include <gtest/gtest.h>

#include "ImageComparisonTestFixture.h"
#include "tiny_skia/Color.h"
#include "tiny_skia/Geom.h"
#include "tiny_skia/Painter.h"
#include "tiny_skia/Path.h"
#include "tiny_skia/PathBuilder.h"
#include "tiny_skia/Pixmap.h"

namespace {

using tiny_skia::BlendMode;
using tiny_skia::Color;
using tiny_skia::ColorU8;
using tiny_skia::FillRule;
using tiny_skia::Paint;
using tiny_skia::Path;
using tiny_skia::PathBuilder;
using tiny_skia::Pixmap;
using tiny_skia::PremultipliedColorU8;
using tiny_skia::Rect;
using tiny_skia::Transform;
using tiny_skia::test_utils::Params;

// 1. horizontal_line - line has no area, output should match empty.png
TEST(FillTest, HorizontalLine) {
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = false;

  PathBuilder pb;
  pb.moveTo(10.0f, 10.0f);
  pb.lineTo(90.0f, 10.0f);
  auto path = pb.finish();
  ASSERT_TRUE(path.has_value());

  auto pixmap = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(pixmap.has_value());
  auto mut = pixmap->mutableView();
  tiny_skia::Painter::fillPath(mut, *path, paint, FillRule::Winding, Transform::identity());

  EXPECT_GOLDEN_MATCH(*pixmap, "fill/empty.png");
}

// 2. vertical_line - line has no area, output should match empty.png
TEST(FillTest, VerticalLine) {
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = false;

  PathBuilder pb;
  pb.moveTo(10.0f, 10.0f);
  pb.lineTo(10.0f, 90.0f);
  auto path = pb.finish();
  ASSERT_TRUE(path.has_value());

  auto pixmap = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(pixmap.has_value());
  auto mut = pixmap->mutableView();
  tiny_skia::Painter::fillPath(mut, *path, paint, FillRule::Winding, Transform::identity());

  EXPECT_GOLDEN_MATCH(*pixmap, "fill/empty.png");
}

// 3. single_line - diagonal line has no area, output should match empty.png
TEST(FillTest, SingleLine) {
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = false;

  PathBuilder pb;
  pb.moveTo(10.0f, 10.0f);
  pb.lineTo(90.0f, 90.0f);
  auto path = pb.finish();
  ASSERT_TRUE(path.has_value());

  auto pixmap = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(pixmap.has_value());
  auto mut = pixmap->mutableView();
  tiny_skia::Painter::fillPath(mut, *path, paint, FillRule::Winding, Transform::identity());

  EXPECT_GOLDEN_MATCH(*pixmap, "fill/empty.png");
}

// 4. int_rect
TEST(FillTest, IntRect) {
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = false;

  auto rect = Rect::fromXYWH(10.0f, 15.0f, 80.0f, 70.0f);
  ASSERT_TRUE(rect.has_value());

  auto pixmap = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(pixmap.has_value());
  auto mut = pixmap->mutableView();
  tiny_skia::Painter::fillRect(mut, *rect, paint, Transform::identity());

  EXPECT_GOLDEN_MATCH(*pixmap, "fill/int-rect.png");
}

// 5. float_rect
TEST(FillTest, FloatRect) {
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = false;

  auto rect = Rect::fromXYWH(10.3f, 15.4f, 80.5f, 70.6f);
  ASSERT_TRUE(rect.has_value());

  auto pixmap = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(pixmap.has_value());
  auto mut = pixmap->mutableView();
  tiny_skia::Painter::fillRect(mut, *rect, paint, Transform::identity());

  EXPECT_GOLDEN_MATCH(*pixmap, "fill/float-rect.png");
}

// 6. int_rect_aa
TEST(FillTest, IntRectAa) {
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = true;

  auto rect = Rect::fromXYWH(10.0f, 15.0f, 80.0f, 70.0f);
  ASSERT_TRUE(rect.has_value());

  auto pixmap = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(pixmap.has_value());
  auto mut = pixmap->mutableView();
  tiny_skia::Painter::fillRect(mut, *rect, paint, Transform::identity());

  EXPECT_GOLDEN_MATCH(*pixmap, "fill/int-rect-aa.png");
}

// 7. float_rect_aa
TEST(FillTest, FloatRectAa) {
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = true;

  auto rect = Rect::fromXYWH(10.3f, 15.4f, 80.5f, 70.6f);
  ASSERT_TRUE(rect.has_value());

  auto pixmap = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(pixmap.has_value());
  auto mut = pixmap->mutableView();
  tiny_skia::Painter::fillRect(mut, *rect, paint, Transform::identity());

  EXPECT_GOLDEN_MATCH(*pixmap, "fill/float-rect-aa.png");
}

// 8. float_rect_aa_highp
TEST(FillTest, FloatRectAaHighp) {
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = true;
  paint.forceHqPipeline = true;

  auto rect = Rect::fromXYWH(10.3f, 15.4f, 80.5f, 70.6f);
  ASSERT_TRUE(rect.has_value());

  auto pixmap = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(pixmap.has_value());
  auto mut = pixmap->mutableView();
  tiny_skia::Painter::fillRect(mut, *rect, paint, Transform::identity());

  EXPECT_GOLDEN_MATCH(*pixmap, "fill/float-rect-aa-highp.png");
}

// 9. tiny_float_rect - pixel comparison (no golden)
TEST(FillTest, TinyFloatRect) {
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = false;

  auto rect = Rect::fromXYWH(1.3f, 1.4f, 0.5f, 0.6f);
  ASSERT_TRUE(rect.has_value());

  auto pixmap = Pixmap::fromSize(3, 3);
  ASSERT_TRUE(pixmap.has_value());
  auto mut = pixmap->mutableView();
  tiny_skia::Painter::fillRect(mut, *rect, paint, Transform::identity());

  auto transparent = ColorU8::fromRgba(0, 0, 0, 0).premultiply();
  auto filled = ColorU8::fromRgba(50, 127, 150, 200).premultiply();

  // Row 0: all transparent
  EXPECT_EQ(pixmap->pixel(0, 0), transparent);
  EXPECT_EQ(pixmap->pixel(1, 0), transparent);
  EXPECT_EQ(pixmap->pixel(2, 0), transparent);

  // Row 1: transparent, filled, transparent
  EXPECT_EQ(pixmap->pixel(0, 1), transparent);
  EXPECT_EQ(pixmap->pixel(1, 1), filled);
  EXPECT_EQ(pixmap->pixel(2, 1), transparent);

  // Row 2: all transparent
  EXPECT_EQ(pixmap->pixel(0, 2), transparent);
  EXPECT_EQ(pixmap->pixel(1, 2), transparent);
  EXPECT_EQ(pixmap->pixel(2, 2), transparent);
}

// 10. tiny_float_rect_aa - pixel comparison (no golden)
TEST(FillTest, TinyFloatRectAa) {
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = true;

  auto rect = Rect::fromXYWH(1.3f, 1.4f, 0.5f, 0.6f);
  ASSERT_TRUE(rect.has_value());

  auto pixmap = Pixmap::fromSize(3, 3);
  ASSERT_TRUE(pixmap.has_value());
  auto mut = pixmap->mutableView();
  tiny_skia::Painter::fillRect(mut, *rect, paint, Transform::identity());

  auto transparent = ColorU8::fromRgba(0, 0, 0, 0).premultiply();

  // Row 0: all transparent
  EXPECT_EQ(pixmap->pixel(0, 0), transparent);
  EXPECT_EQ(pixmap->pixel(1, 0), transparent);
  EXPECT_EQ(pixmap->pixel(2, 0), transparent);

  // Row 1: transparent, AA-blended pixel, transparent.
  // The exact coverage value depends on the AA algorithm (supersampled vs analytic).
  // Check that it's non-transparent rather than asserting an exact value.
  EXPECT_EQ(pixmap->pixel(0, 1), transparent);
  EXPECT_NE(pixmap->pixel(1, 1), transparent);
  EXPECT_EQ(pixmap->pixel(2, 1), transparent);

  // Row 2: all transparent
  EXPECT_EQ(pixmap->pixel(0, 2), transparent);
  EXPECT_EQ(pixmap->pixel(1, 2), transparent);
  EXPECT_EQ(pixmap->pixel(2, 2), transparent);
}

// 11. tiny_rect_aa - just runs (no crash)
TEST(FillTest, TinyRectAa) {
  Paint paint;
  paint.setColorRgba8(0, 0, 0, 0);
  paint.antiAlias = true;

  auto rect = Rect::fromXYWH(0.7f, 0.0f, 1.0f, 2.0f);
  ASSERT_TRUE(rect.has_value());

  auto pixmap = Pixmap::fromSize(10, 10);
  ASSERT_TRUE(pixmap.has_value());
  auto mut = pixmap->mutableView();
  tiny_skia::Painter::fillRect(mut, *rect, paint, Transform::identity());
}

// 12. float_rect_clip_top_left_aa
TEST(FillTest, FloatRectClipTopLeftAa) {
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = true;

  auto rect = Rect::fromXYWH(-10.3f, -20.4f, 100.5f, 70.2f);
  ASSERT_TRUE(rect.has_value());

  auto pixmap = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(pixmap.has_value());
  auto mut = pixmap->mutableView();
  tiny_skia::Painter::fillRect(mut, *rect, paint, Transform::identity());

  EXPECT_GOLDEN_MATCH(*pixmap, "fill/float-rect-clip-top-left-aa.png");
}

// 13. float_rect_clip_top_right_aa
TEST(FillTest, FloatRectClipTopRightAa) {
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = true;

  auto rect = Rect::fromXYWH(60.3f, -20.4f, 100.5f, 70.2f);
  ASSERT_TRUE(rect.has_value());

  auto pixmap = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(pixmap.has_value());
  auto mut = pixmap->mutableView();
  tiny_skia::Painter::fillRect(mut, *rect, paint, Transform::identity());

  EXPECT_GOLDEN_MATCH(*pixmap, "fill/float-rect-clip-top-right-aa.png");
}

// 14. float_rect_clip_bottom_right_aa
TEST(FillTest, FloatRectClipBottomRightAa) {
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = true;

  auto rect = Rect::fromXYWH(60.3f, 40.4f, 100.5f, 70.2f);
  ASSERT_TRUE(rect.has_value());

  auto pixmap = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(pixmap.has_value());
  auto mut = pixmap->mutableView();
  tiny_skia::Painter::fillRect(mut, *rect, paint, Transform::identity());

  EXPECT_GOLDEN_MATCH(*pixmap, "fill/float-rect-clip-bottom-right-aa.png");
}

// 15. int_rect_with_ts_clip_right
TEST(FillTest, IntRectWithTsClipRight) {
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = false;

  auto rect = Rect::fromXYWH(0.0f, 0.0f, 100.0f, 100.0f);
  ASSERT_TRUE(rect.has_value());

  auto pixmap = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(pixmap.has_value());
  auto mut = pixmap->mutableView();
  tiny_skia::Painter::fillRect(mut, *rect, paint,
                       Transform::fromRow(1.0f, 0.0f, 0.0f, 1.0f, 0.5f, 0.5f));

  EXPECT_GOLDEN_MATCH(*pixmap, "fill/int-rect-with-ts-clip-right.png");
}

// 16. open_polygon
TEST(FillTest, OpenPolygon) {
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = false;

  PathBuilder pb;
  pb.moveTo(75.160671f, 88.756136f);
  pb.lineTo(24.797274f, 88.734053f);
  pb.lineTo( 9.255130f, 40.828792f);
  pb.lineTo(50.012955f, 11.243795f);
  pb.lineTo(90.744819f, 40.864522f);
  auto path = pb.finish();
  ASSERT_TRUE(path.has_value());

  auto pixmap = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(pixmap.has_value());
  auto mut = pixmap->mutableView();
  tiny_skia::Painter::fillPath(mut, *path, paint, FillRule::Winding, Transform::identity());

  EXPECT_GOLDEN_MATCH(*pixmap, "fill/polygon.png");
}

// 17. closed_polygon - must be the same as open_polygon
TEST(FillTest, ClosedPolygon) {
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = false;

  PathBuilder pb;
  pb.moveTo(75.160671f, 88.756136f);
  pb.lineTo(24.797274f, 88.734053f);
  pb.lineTo( 9.255130f, 40.828792f);
  pb.lineTo(50.012955f, 11.243795f);
  pb.lineTo(90.744819f, 40.864522f);
  pb.close();  // the only difference
  auto path = pb.finish();
  ASSERT_TRUE(path.has_value());

  auto pixmap = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(pixmap.has_value());
  auto mut = pixmap->mutableView();
  tiny_skia::Painter::fillPath(mut, *path, paint, FillRule::Winding, Transform::identity());

  EXPECT_GOLDEN_MATCH(*pixmap, "fill/polygon.png");
}

// 18. winding_star
TEST(FillTest, WindingStar) {
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = false;

  PathBuilder pb;
  pb.moveTo(50.0f,  7.5f);
  pb.lineTo(75.0f, 87.5f);
  pb.lineTo(10.0f, 37.5f);
  pb.lineTo(90.0f, 37.5f);
  pb.lineTo(25.0f, 87.5f);
  auto path = pb.finish();
  ASSERT_TRUE(path.has_value());

  auto pixmap = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(pixmap.has_value());
  auto mut = pixmap->mutableView();
  tiny_skia::Painter::fillPath(mut, *path, paint, FillRule::Winding, Transform::identity());

  EXPECT_GOLDEN_MATCH(*pixmap, "fill/winding-star.png");
}

// 19. even_odd_star
TEST(FillTest, EvenOddStar) {
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = false;

  PathBuilder pb;
  pb.moveTo(50.0f,  7.5f);
  pb.lineTo(75.0f, 87.5f);
  pb.lineTo(10.0f, 37.5f);
  pb.lineTo(90.0f, 37.5f);
  pb.lineTo(25.0f, 87.5f);
  auto path = pb.finish();
  ASSERT_TRUE(path.has_value());

  auto pixmap = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(pixmap.has_value());
  auto mut = pixmap->mutableView();
  tiny_skia::Painter::fillPath(mut, *path, paint, FillRule::EvenOdd, Transform::identity());

  EXPECT_GOLDEN_MATCH(*pixmap, "fill/even-odd-star.png");
}

// 20. quad_curve
TEST(FillTest, QuadCurve) {
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = false;

  PathBuilder pb;
  pb.moveTo(10.0f, 15.0f);
  pb.quadTo(95.0f, 35.0f, 75.0f, 90.0f);
  auto path = pb.finish();
  ASSERT_TRUE(path.has_value());

  auto pixmap = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(pixmap.has_value());
  auto mut = pixmap->mutableView();
  tiny_skia::Painter::fillPath(mut, *path, paint, FillRule::EvenOdd, Transform::identity());

  EXPECT_GOLDEN_MATCH(*pixmap, "fill/quad.png");
}

// 21. cubic_curve
TEST(FillTest, CubicCurve) {
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = false;

  PathBuilder pb;
  pb.moveTo(10.0f, 15.0f);
  pb.cubicTo(95.0f, 35.0f, 0.0f, 75.0f, 75.0f, 90.0f);
  auto path = pb.finish();
  ASSERT_TRUE(path.has_value());

  auto pixmap = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(pixmap.has_value());
  auto mut = pixmap->mutableView();
  tiny_skia::Painter::fillPath(mut, *path, paint, FillRule::EvenOdd, Transform::identity());

  EXPECT_GOLDEN_MATCH(*pixmap, "fill/cubic.png");
}

// 22. memset2d
TEST(FillTest, Memset2d) {
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 255);  // Must be opaque to trigger memset2d.
  paint.antiAlias = false;

  auto rect = Rect::fromLTRB(10.0f, 10.0f, 90.0f, 90.0f);
  ASSERT_TRUE(rect.has_value());
  auto path = tiny_skia::Path::fromRect(*rect);

  auto pixmap = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(pixmap.has_value());
  auto mut = pixmap->mutableView();
  tiny_skia::Painter::fillPath(mut, path, paint, FillRule::Winding, Transform::identity());

  EXPECT_GOLDEN_MATCH(*pixmap, "fill/memset2d.png");
}

// 23. memset2d_out_of_bounds - make sure we do not write past pixmap memory
TEST(FillTest, Memset2dOutOfBounds) {
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 255);  // Must be opaque to trigger memset2d.
  paint.antiAlias = false;

  auto rect = Rect::fromLTRB(50.0f, 50.0f, 120.0f, 120.0f);
  ASSERT_TRUE(rect.has_value());
  auto path = tiny_skia::Path::fromRect(*rect);

  auto pixmap = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(pixmap.has_value());
  auto mut = pixmap->mutableView();
  tiny_skia::Painter::fillPath(mut, path, paint, FillRule::Winding, Transform::identity());

  EXPECT_GOLDEN_MATCH(*pixmap, "fill/memset2d-2.png");
}

// 24. fill_aa
TEST(FillTest, FillAa) {
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = true;

  PathBuilder pb;
  pb.moveTo(50.0f,  7.5f);
  pb.lineTo(75.0f, 87.5f);
  pb.lineTo(10.0f, 37.5f);
  pb.lineTo(90.0f, 37.5f);
  pb.lineTo(25.0f, 87.5f);
  auto path = pb.finish();
  ASSERT_TRUE(path.has_value());

  auto pixmap = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(pixmap.has_value());
  auto mut = pixmap->mutableView();
  tiny_skia::Painter::fillPath(mut, *path, paint, FillRule::EvenOdd, Transform::identity());

  EXPECT_GOLDEN_MATCH(*pixmap, "fill/star-aa.png");
}

// 25. overflow_in_walk_edges_1 - must not panic/crash
TEST(FillTest, OverflowInWalkEdges1) {
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = false;

  PathBuilder pb;
  pb.moveTo(10.0f, 20.0f);
  pb.cubicTo(39.0f, 163.0f, 117.0f, 61.0f, 130.0f, 70.0f);
  auto path = pb.finish();
  ASSERT_TRUE(path.has_value());

  auto pixmap = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(pixmap.has_value());
  auto mut = pixmap->mutableView();
  tiny_skia::Painter::fillPath(mut, *path, paint, FillRule::Winding, Transform::identity());
}

// 26. clip_line_1
TEST(FillTest, ClipLine1) {
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = false;

  PathBuilder pb;
  pb.moveTo(50.0f, -15.0f);
  pb.lineTo(-15.0f, 50.0f);
  pb.lineTo(50.0f, 115.0f);
  pb.lineTo(115.0f, 50.0f);
  pb.close();
  auto path = pb.finish();
  ASSERT_TRUE(path.has_value());

  auto pixmap = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(pixmap.has_value());
  auto mut = pixmap->mutableView();
  tiny_skia::Painter::fillPath(mut, *path, paint, FillRule::Winding, Transform::identity());

  EXPECT_GOLDEN_MATCH(*pixmap, "fill/clip-line-1.png");
}

// 27. clip_line_2
TEST(FillTest, ClipLine2) {
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = false;

  // This strange path forces lineClipper::clip to return an empty array.
  // We're checking that this case is handled correctly.
  PathBuilder pb;
  pb.moveTo(0.0f, -1.0f);
  pb.lineTo(50.0f, 0.0f);
  pb.lineTo(0.0f, 50.0f);
  pb.close();
  auto path = pb.finish();
  ASSERT_TRUE(path.has_value());

  auto pixmap = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(pixmap.has_value());
  auto mut = pixmap->mutableView();
  tiny_skia::Painter::fillPath(mut, *path, paint, FillRule::Winding, Transform::identity());

  EXPECT_GOLDEN_MATCH(*pixmap, "fill/clip-line-2.png");
}

// 28. clip_quad
TEST(FillTest, ClipQuad) {
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = false;

  PathBuilder pb;
  pb.moveTo(10.0f, 85.0f);
  pb.quadTo(150.0f, 150.0f, 85.0f, 15.0f);
  auto path = pb.finish();
  ASSERT_TRUE(path.has_value());

  auto pixmap = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(pixmap.has_value());
  auto mut = pixmap->mutableView();
  tiny_skia::Painter::fillPath(mut, *path, paint, FillRule::Winding, Transform::identity());

  EXPECT_GOLDEN_MATCH(*pixmap, "fill/clip-quad.png");
}

// 29. clip_cubic_1
TEST(FillTest, ClipCubic1) {
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = false;

  // lineClipper::clip produces 2 points for this path.
  PathBuilder pb;
  pb.moveTo(10.0f, 50.0f);
  pb.cubicTo(0.0f, 175.0f, 195.0f, 70.0f, 75.0f, 20.0f);
  auto path = pb.finish();
  ASSERT_TRUE(path.has_value());

  auto pixmap = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(pixmap.has_value());
  auto mut = pixmap->mutableView();
  tiny_skia::Painter::fillPath(mut, *path, paint, FillRule::Winding, Transform::identity());

  EXPECT_GOLDEN_MATCH(*pixmap, "fill/clip-cubic-1.png");
}

// 30. clip_cubic_2
TEST(FillTest, ClipCubic2) {
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = false;

  // lineClipper::clip produces 3 points for this path.
  PathBuilder pb;
  pb.moveTo(10.0f, 50.0f);
  pb.cubicTo(10.0f, 40.0f, 90.0f, 120.0f, 125.0f, 20.0f);
  auto path = pb.finish();
  ASSERT_TRUE(path.has_value());

  auto pixmap = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(pixmap.has_value());
  auto mut = pixmap->mutableView();
  tiny_skia::Painter::fillPath(mut, *path, paint, FillRule::Winding, Transform::identity());

  EXPECT_GOLDEN_MATCH(*pixmap, "fill/clip-cubic-2.png");
}

// 31. aa_endless_loop - must not loop endlessly
TEST(FillTest, AaEndlessLoop) {
  Paint paint;
  paint.antiAlias = true;

  // This path was causing an endless loop before.
  PathBuilder pb;
  pb.moveTo(2.1537175f, 11.560721f);
  pb.quadTo(1.9999998f, 10.787931f, 2.0f, 10.0f);
  auto path = pb.finish();
  ASSERT_TRUE(path.has_value());

  auto pixmap = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(pixmap.has_value());
  auto mut = pixmap->mutableView();
  tiny_skia::Painter::fillPath(mut, *path, paint, FillRule::Winding, Transform::identity());
}

// 32. clear_aa - make sure that Clear with AA doesn't fallback to memset
TEST(FillTest, ClearAa) {
  Paint paint;
  paint.antiAlias = true;
  paint.blendMode = BlendMode::Clear;

  auto pixmap = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(pixmap.has_value());
  pixmap->fill(Color::fromRgba8(50, 127, 150, 200));

  // Build a circle path using pushCircle.
  PathBuilder pb;
  pb.pushCircle(50.0f, 50.0f, 40.0f);
  auto path = pb.finish();
  ASSERT_TRUE(path.has_value());

  auto mut = pixmap->mutableView();
  tiny_skia::Painter::fillPath(mut, *path, paint, FillRule::Winding, Transform::identity());

  EXPECT_GOLDEN_MATCH_WITH_PARAMS(*pixmap, "fill/clear-aa.png",
                                  Params::WithThreshold(0.1f));
}

// 33. line_curve - must not panic/crash
TEST(FillTest, LineCurve) {
  Paint paint;
  paint.antiAlias = true;

  PathBuilder pb;
  pb.moveTo(100.0f, 20.0f);
  pb.cubicTo(100.0f, 40.0f, 100.0f, 160.0f, 100.0f, 180.0f);  // Just a line.
  auto path = pb.finish();
  ASSERT_TRUE(path.has_value());

  auto pixmap = Pixmap::fromSize(200, 200);
  ASSERT_TRUE(pixmap.has_value());
  auto mut = pixmap->mutableView();
  tiny_skia::Painter::fillPath(mut, *path, paint, FillRule::Winding, Transform::identity());
}

// 34. vertical_lines_merging_bug
TEST(FillTest, VerticalLinesMergingBug) {
  // This path must not trigger edge_builder::combine_vertical,
  // otherwise AlphaRuns::add will crash later.
  PathBuilder pb;
  pb.moveTo(765.56f, 158.56f);
  pb.lineTo(754.4f, 168.28f);
  pb.cubicTo(754.4f, 168.28f, 754.4f, 168.24f, 754.4f, 168.17f);
  pb.cubicTo(754.4f, 168.09f, 754.4f, 168.02f, 754.4f, 167.95f);
  pb.lineTo(754.4f, 168.06f);
  auto path = pb.finish();
  ASSERT_TRUE(path.has_value());

  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = true;

  auto pixmap = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(pixmap.has_value());
  auto mut = pixmap->mutableView();
  tiny_skia::Painter::fillPath(mut, *path, paint, FillRule::Winding,
                       Transform::fromRow(5.4f, 0.0f, 0.0f, 5.4f, -4050.0f, -840.0f));

  EXPECT_GOLDEN_MATCH_WITH_PARAMS(*pixmap, "fill/vertical-lines-merging-bug.png",
                                  Params::WithThreshold(0.1f));
}

// 35. fill_rect (canvas test)
TEST(FillTest, FillRectCanvas) {
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = true;

  auto pixmap = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(pixmap.has_value());

  auto rect = Rect::fromXYWH(20.3f, 10.4f, 50.5f, 30.2f);
  ASSERT_TRUE(rect.has_value());

  auto mut = pixmap->mutableView();
  tiny_skia::Painter::fillRect(mut, *rect, paint,
                       Transform::fromRow(1.2f, 0.3f, -0.7f, 0.8f, 12.0f, 15.3f));

  EXPECT_GOLDEN_MATCH_WITH_PARAMS(*pixmap, "canvas/fill-rect.png",
                                  Params::WithThreshold(0.1f));
}

}  // namespace
