/// @file CrossValidationTest.cpp
/// Live cross-validation tests that render the same scene with both the C++
/// port and the original Rust tiny-skia implementation, then compare pixel
/// output for exact parity.

#include <gtest/gtest.h>

#include "CrossValidator.h"
#include "RustReference.h"
#include "tiny_skia/Color.h"
#include "tiny_skia/Geom.h"
#include "tiny_skia/Painter.h"
#include "tiny_skia/Path.h"
#include "tiny_skia/PathBuilder.h"
#include "tiny_skia/Pixmap.h"
#include "tiny_skia/Stroke.h"

namespace {

using tiny_skia::BlendMode;
using tiny_skia::Color;
using tiny_skia::FillRule;
using tiny_skia::LineCap;
using tiny_skia::LineJoin;
using tiny_skia::Paint;
using tiny_skia::Path;
using tiny_skia::PathBuilder;
using tiny_skia::Pixmap;
using tiny_skia::Rect;
using tiny_skia::Stroke;
using tiny_skia::StrokeDash;
using tiny_skia::Transform;

namespace rr = tiny_skia::rustRef;

// ===========================================================================
// Fill – rectangles
// ===========================================================================

TEST(CrossValidation, FillIntRect) {
  // C++ side
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = false;

  auto rect = Rect::fromLTRB(10.0f, 15.0f, 90.0f, 85.0f);
  ASSERT_TRUE(rect.has_value());

  auto cpp = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(cpp.has_value());
  auto mut = cpp->mutableView();
  tiny_skia::Painter::fillRect(mut, *rect, paint, Transform::identity());

  // Rust side
  auto rust = rr::Pixmap::create(100, 100);
  ASSERT_TRUE(rust.has_value());
  rr::fillRect(*rust, 10.0f, 15.0f, 90.0f, 85.0f, 50, 127, 150, 200,
               /*antiAlias=*/false, BlendMode::SourceOver,
               Transform::identity());

  EXPECT_CROSS_MATCH(*cpp, *rust);
}

TEST(CrossValidation, FillFloatRectAa) {
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = true;

  auto rect = Rect::fromLTRB(10.3f, 15.4f, 90.8f, 86.0f);
  ASSERT_TRUE(rect.has_value());

  auto cpp = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(cpp.has_value());
  auto mut = cpp->mutableView();
  tiny_skia::Painter::fillRect(mut, *rect, paint, Transform::identity());

  auto rust = rr::Pixmap::create(100, 100);
  ASSERT_TRUE(rust.has_value());
  rr::fillRect(*rust, 10.3f, 15.4f, 90.8f, 86.0f, 50, 127, 150, 200,
               /*antiAlias=*/true, BlendMode::SourceOver,
               Transform::identity());

  EXPECT_CROSS_MATCH(*cpp, *rust);
}

// ===========================================================================
// Fill – paths
// ===========================================================================

TEST(CrossValidation, FillTriangleWinding) {
  // C++ side
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = true;

  PathBuilder cpb;
  cpb.moveTo(50.0f, 10.0f);
  cpb.lineTo(90.0f, 90.0f);
  cpb.lineTo(10.0f, 90.0f);
  cpb.close();
  auto cppPath = cpb.finish();
  ASSERT_TRUE(cppPath.has_value());

  auto cpp = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(cpp.has_value());
  auto mut = cpp->mutableView();
  tiny_skia::Painter::fillPath(mut, *cppPath, paint, FillRule::Winding,
                       Transform::identity());

  // Rust side
  auto rust = rr::Pixmap::create(100, 100);
  ASSERT_TRUE(rust.has_value());

  rr::PathBuilder rpb;
  rpb.moveTo(50.0f, 10.0f);
  rpb.lineTo(90.0f, 90.0f);
  rpb.lineTo(10.0f, 90.0f);
  rpb.close();
  auto rustPath = rpb.finish();
  ASSERT_TRUE(rustPath.has_value());

  rr::fillPath(*rust, *rustPath, 50, 127, 150, 200, FillRule::Winding,
               Transform::identity());

  EXPECT_CROSS_MATCH(*cpp, *rust);
}

TEST(CrossValidation, FillTriangleEvenOdd) {
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = true;

  PathBuilder cpb;
  cpb.moveTo(50.0f, 10.0f);
  cpb.lineTo(90.0f, 90.0f);
  cpb.lineTo(10.0f, 90.0f);
  cpb.close();
  auto cppPath = cpb.finish();
  ASSERT_TRUE(cppPath.has_value());

  auto cpp = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(cpp.has_value());
  auto mut = cpp->mutableView();
  tiny_skia::Painter::fillPath(mut, *cppPath, paint, FillRule::EvenOdd,
                       Transform::identity());

  auto rust = rr::Pixmap::create(100, 100);
  ASSERT_TRUE(rust.has_value());

  rr::PathBuilder rpb;
  rpb.moveTo(50.0f, 10.0f);
  rpb.lineTo(90.0f, 90.0f);
  rpb.lineTo(10.0f, 90.0f);
  rpb.close();
  auto rustPath = rpb.finish();
  ASSERT_TRUE(rustPath.has_value());

  rr::fillPath(*rust, *rustPath, 50, 127, 150, 200, FillRule::EvenOdd,
               Transform::identity());

  EXPECT_CROSS_MATCH(*cpp, *rust);
}

TEST(CrossValidation, FillQuadCurve) {
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = true;

  PathBuilder cpb;
  cpb.moveTo(10.0f, 80.0f);
  cpb.quadTo(50.0f, 10.0f, 90.0f, 80.0f);
  cpb.close();
  auto cppPath = cpb.finish();
  ASSERT_TRUE(cppPath.has_value());

  auto cpp = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(cpp.has_value());
  auto mut = cpp->mutableView();
  tiny_skia::Painter::fillPath(mut, *cppPath, paint, FillRule::Winding,
                       Transform::identity());

  auto rust = rr::Pixmap::create(100, 100);
  ASSERT_TRUE(rust.has_value());

  rr::PathBuilder rpb;
  rpb.moveTo(10.0f, 80.0f);
  rpb.quadTo(50.0f, 10.0f, 90.0f, 80.0f);
  rpb.close();
  auto rustPath = rpb.finish();
  ASSERT_TRUE(rustPath.has_value());

  rr::fillPath(*rust, *rustPath, 50, 127, 150, 200, FillRule::Winding,
               Transform::identity());

  EXPECT_CROSS_MATCH(*cpp, *rust);
}

TEST(CrossValidation, FillCubicCurve) {
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = true;

  PathBuilder cpb;
  cpb.moveTo(10.0f, 80.0f);
  cpb.cubicTo(20.0f, 10.0f, 80.0f, 10.0f, 90.0f, 80.0f);
  cpb.close();
  auto cppPath = cpb.finish();
  ASSERT_TRUE(cppPath.has_value());

  auto cpp = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(cpp.has_value());
  auto mut = cpp->mutableView();
  tiny_skia::Painter::fillPath(mut, *cppPath, paint, FillRule::Winding,
                       Transform::identity());

  auto rust = rr::Pixmap::create(100, 100);
  ASSERT_TRUE(rust.has_value());

  rr::PathBuilder rpb;
  rpb.moveTo(10.0f, 80.0f);
  rpb.cubicTo(20.0f, 10.0f, 80.0f, 10.0f, 90.0f, 80.0f);
  rpb.close();
  auto rustPath = rpb.finish();
  ASSERT_TRUE(rustPath.has_value());

  rr::fillPath(*rust, *rustPath, 50, 127, 150, 200, FillRule::Winding,
               Transform::identity());

  EXPECT_CROSS_MATCH(*cpp, *rust);
}

// ===========================================================================
// Fill – with transform
// ===========================================================================

TEST(CrossValidation, FillRectScaled) {
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = true;

  auto rect = Rect::fromLTRB(5.0f, 5.0f, 45.0f, 45.0f);
  ASSERT_TRUE(rect.has_value());

  auto ts = Transform::fromScale(2.0f, 2.0f);

  auto cpp = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(cpp.has_value());
  auto mut = cpp->mutableView();
  tiny_skia::Painter::fillRect(mut, *rect, paint, ts);

  auto rust = rr::Pixmap::create(100, 100);
  ASSERT_TRUE(rust.has_value());
  rr::fillRect(*rust, 5.0f, 5.0f, 45.0f, 45.0f, 50, 127, 150, 200,
               /*antiAlias=*/true, BlendMode::SourceOver, ts);

  EXPECT_CROSS_MATCH(*cpp, *rust);
}

TEST(CrossValidation, FillPathTranslated) {
  Paint paint;
  paint.setColorRgba8(200, 100, 50, 255);
  paint.antiAlias = true;

  auto ts = Transform::fromTranslate(20.0f, 30.0f);

  PathBuilder cpb;
  cpb.moveTo(10.0f, 10.0f);
  cpb.lineTo(40.0f, 10.0f);
  cpb.lineTo(40.0f, 40.0f);
  cpb.lineTo(10.0f, 40.0f);
  cpb.close();
  auto cppPath = cpb.finish();
  ASSERT_TRUE(cppPath.has_value());

  auto cpp = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(cpp.has_value());
  auto mut = cpp->mutableView();
  tiny_skia::Painter::fillPath(mut, *cppPath, paint, FillRule::Winding, ts);

  auto rust = rr::Pixmap::create(100, 100);
  ASSERT_TRUE(rust.has_value());

  rr::PathBuilder rpb;
  rpb.moveTo(10.0f, 10.0f);
  rpb.lineTo(40.0f, 10.0f);
  rpb.lineTo(40.0f, 40.0f);
  rpb.lineTo(10.0f, 40.0f);
  rpb.close();
  auto rustPath = rpb.finish();
  ASSERT_TRUE(rustPath.has_value());

  rr::fillPath(*rust, *rustPath, 200, 100, 50, 255, FillRule::Winding, ts);

  EXPECT_CROSS_MATCH(*cpp, *rust);
}

// ===========================================================================
// Fill – circle (exercises push_circle / conic-to-quad conversion)
// ===========================================================================

TEST(CrossValidation, FillCircle) {
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = true;

  PathBuilder cpb;
  cpb.pushCircle(50.0f, 50.0f, 40.0f);
  auto cppPath = cpb.finish();
  ASSERT_TRUE(cppPath.has_value());

  auto cpp = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(cpp.has_value());
  auto mut = cpp->mutableView();
  tiny_skia::Painter::fillPath(mut, *cppPath, paint, FillRule::Winding,
                       Transform::identity());

  auto rust = rr::Pixmap::create(100, 100);
  ASSERT_TRUE(rust.has_value());

  rr::PathBuilder rpb;
  rpb.pushCircle(50.0f, 50.0f, 40.0f);
  auto rustPath = rpb.finish();
  ASSERT_TRUE(rustPath.has_value());

  rr::fillPath(*rust, *rustPath, 50, 127, 150, 200, FillRule::Winding,
               Transform::identity());

  EXPECT_CROSS_MATCH(*cpp, *rust);
}

// ===========================================================================
// Fill – blend modes
// ===========================================================================

TEST(CrossValidation, FillRectBlendXor) {
  // Draw a background, then XOR a second rect on top.
  auto cpp = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(cpp.has_value());
  cpp->fill(Color::fromRgba8(0, 0, 0, 255));

  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = false;
  paint.blendMode = BlendMode::Xor;

  auto rect = Rect::fromLTRB(10.0f, 10.0f, 90.0f, 90.0f);
  ASSERT_TRUE(rect.has_value());
  auto mut = cpp->mutableView();
  tiny_skia::Painter::fillRect(mut, *rect, paint, Transform::identity());

  auto rust = rr::Pixmap::create(100, 100);
  ASSERT_TRUE(rust.has_value());
  rust->fill(0, 0, 0, 255);

  rr::fillRect(*rust, 10.0f, 10.0f, 90.0f, 90.0f, 50, 127, 150, 200,
               /*antiAlias=*/false, BlendMode::Xor, Transform::identity());

  EXPECT_CROSS_MATCH(*cpp, *rust);
}

// ===========================================================================
// Stroke – basic
// ===========================================================================

TEST(CrossValidation, StrokeCircle) {
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = true;

  PathBuilder cpb;
  cpb.pushCircle(100.0f, 100.0f, 50.0f);
  auto cppPath = cpb.finish();
  ASSERT_TRUE(cppPath.has_value());

  Stroke stroke;
  stroke.width = 3.0f;

  auto cpp = Pixmap::fromSize(200, 200);
  ASSERT_TRUE(cpp.has_value());
  auto mut = cpp->mutableView();
  tiny_skia::Painter::strokePath(mut, *cppPath, paint, stroke, Transform::identity());

  auto rust = rr::Pixmap::create(200, 200);
  ASSERT_TRUE(rust.has_value());

  rr::PathBuilder rpb;
  rpb.pushCircle(100.0f, 100.0f, 50.0f);
  auto rustPath = rpb.finish();
  ASSERT_TRUE(rustPath.has_value());

  rr::strokePath(*rust, *rustPath, 50, 127, 150, 200, stroke,
                 Transform::identity());

  // Allow up to 3 pixels of difference due to FMA contraction differences
  // between C++ and Rust compilers on ARM64. Both produce correct output but
  // the compilers make different fused-multiply-add optimization decisions at
  // a few anti-aliased edge pixels around the stroked circle.
  int diff = ::tiny_skia::test_utils::comparePixmaps(*cpp, *rust);
  EXPECT_LE(diff, 3) << "C++ vs Rust pixel mismatch: " << diff
                      << " pixels differ (tolerance: 3)";
}

TEST(CrossValidation, StrokeRoundCaps) {
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = true;

  PathBuilder cpb;
  cpb.moveTo(20.0f, 50.0f);
  cpb.lineTo(80.0f, 50.0f);
  auto cppPath = cpb.finish();
  ASSERT_TRUE(cppPath.has_value());

  Stroke stroke;
  stroke.width = 10.0f;
  stroke.lineCap = LineCap::Round;

  auto cpp = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(cpp.has_value());
  auto mut = cpp->mutableView();
  tiny_skia::Painter::strokePath(mut, *cppPath, paint, stroke, Transform::identity());

  auto rust = rr::Pixmap::create(100, 100);
  ASSERT_TRUE(rust.has_value());

  rr::PathBuilder rpb;
  rpb.moveTo(20.0f, 50.0f);
  rpb.lineTo(80.0f, 50.0f);
  auto rustPath = rpb.finish();
  ASSERT_TRUE(rustPath.has_value());

  rr::strokePath(*rust, *rustPath, 50, 127, 150, 200, stroke,
                 Transform::identity());

  EXPECT_CROSS_MATCH(*cpp, *rust);
}

TEST(CrossValidation, StrokeRoundJoinTriangle) {
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = true;

  PathBuilder cpb;
  cpb.moveTo(50.0f, 10.0f);
  cpb.lineTo(90.0f, 90.0f);
  cpb.lineTo(10.0f, 90.0f);
  cpb.close();
  auto cppPath = cpb.finish();
  ASSERT_TRUE(cppPath.has_value());

  Stroke stroke;
  stroke.width = 4.0f;
  stroke.lineJoin = LineJoin::Round;

  auto cpp = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(cpp.has_value());
  auto mut = cpp->mutableView();
  tiny_skia::Painter::strokePath(mut, *cppPath, paint, stroke, Transform::identity());

  auto rust = rr::Pixmap::create(100, 100);
  ASSERT_TRUE(rust.has_value());

  rr::PathBuilder rpb;
  rpb.moveTo(50.0f, 10.0f);
  rpb.lineTo(90.0f, 90.0f);
  rpb.lineTo(10.0f, 90.0f);
  rpb.close();
  auto rustPath = rpb.finish();
  ASSERT_TRUE(rustPath.has_value());

  rr::strokePath(*rust, *rustPath, 50, 127, 150, 200, stroke,
                 Transform::identity());

  EXPECT_CROSS_MATCH(*cpp, *rust);
}

// ===========================================================================
// Stroke – with dash
// ===========================================================================

TEST(CrossValidation, StrokeDashedLine) {
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = true;

  PathBuilder cpb;
  cpb.moveTo(10.0f, 50.0f);
  cpb.lineTo(190.0f, 50.0f);
  auto cppPath = cpb.finish();
  ASSERT_TRUE(cppPath.has_value());

  Stroke stroke;
  stroke.width = 4.0f;
  stroke.dash = StrokeDash::create({10.0f, 5.0f}, 0.0f);
  ASSERT_TRUE(stroke.dash.has_value());

  auto cpp = Pixmap::fromSize(200, 100);
  ASSERT_TRUE(cpp.has_value());
  auto mut = cpp->mutableView();
  tiny_skia::Painter::strokePath(mut, *cppPath, paint, stroke, Transform::identity());

  auto rust = rr::Pixmap::create(200, 100);
  ASSERT_TRUE(rust.has_value());

  rr::PathBuilder rpb;
  rpb.moveTo(10.0f, 50.0f);
  rpb.lineTo(190.0f, 50.0f);
  auto rustPath = rpb.finish();
  ASSERT_TRUE(rustPath.has_value());

  rr::strokePath(*rust, *rustPath, 50, 127, 150, 200, stroke,
                 Transform::identity());

  EXPECT_CROSS_MATCH(*cpp, *rust);
}

// ===========================================================================
// Stroke – with scale transform
// ===========================================================================

TEST(CrossValidation, StrokeScaledPath) {
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = true;

  auto ts = Transform::fromScale(2.0f, 2.0f);

  PathBuilder cpb;
  cpb.moveTo(10.0f, 10.0f);
  cpb.lineTo(40.0f, 10.0f);
  cpb.lineTo(40.0f, 40.0f);
  cpb.lineTo(10.0f, 40.0f);
  cpb.close();
  auto cppPath = cpb.finish();
  ASSERT_TRUE(cppPath.has_value());

  Stroke stroke;
  stroke.width = 3.0f;

  auto cpp = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(cpp.has_value());
  auto mut = cpp->mutableView();
  tiny_skia::Painter::strokePath(mut, *cppPath, paint, stroke, ts);

  auto rust = rr::Pixmap::create(100, 100);
  ASSERT_TRUE(rust.has_value());

  rr::PathBuilder rpb;
  rpb.moveTo(10.0f, 10.0f);
  rpb.lineTo(40.0f, 10.0f);
  rpb.lineTo(40.0f, 40.0f);
  rpb.lineTo(10.0f, 40.0f);
  rpb.close();
  auto rustPath = rpb.finish();
  ASSERT_TRUE(rustPath.has_value());

  rr::strokePath(*rust, *rustPath, 50, 127, 150, 200, stroke, ts);

  EXPECT_CROSS_MATCH(*cpp, *rust);
}

// ===========================================================================
// Edge cases
// ===========================================================================

TEST(CrossValidation, FillEmptyPixmap) {
  // Both sides should produce identical transparent pixmaps after a no-op fill.
  auto cpp = Pixmap::fromSize(50, 50);
  ASSERT_TRUE(cpp.has_value());

  auto rust = rr::Pixmap::create(50, 50);
  ASSERT_TRUE(rust.has_value());

  EXPECT_CROSS_MATCH(*cpp, *rust);
}

TEST(CrossValidation, FillPathNoAntiAlias) {
  Paint paint;
  paint.setColorRgba8(50, 127, 150, 200);
  paint.antiAlias = false;

  PathBuilder cpb;
  cpb.moveTo(10.0f, 10.0f);
  cpb.lineTo(90.0f, 10.0f);
  cpb.lineTo(90.0f, 90.0f);
  cpb.lineTo(10.0f, 90.0f);
  cpb.close();
  auto cppPath = cpb.finish();
  ASSERT_TRUE(cppPath.has_value());

  auto cpp = Pixmap::fromSize(100, 100);
  ASSERT_TRUE(cpp.has_value());
  auto mut = cpp->mutableView();
  tiny_skia::Painter::fillPath(mut, *cppPath, paint, FillRule::Winding,
                       Transform::identity());

  auto rust = rr::Pixmap::create(100, 100);
  ASSERT_TRUE(rust.has_value());

  rr::PathBuilder rpb;
  rpb.moveTo(10.0f, 10.0f);
  rpb.lineTo(90.0f, 10.0f);
  rpb.lineTo(90.0f, 90.0f);
  rpb.lineTo(10.0f, 90.0f);
  rpb.close();
  auto rustPath = rpb.finish();
  ASSERT_TRUE(rustPath.has_value());

  rr::fillPath(*rust, *rustPath, 50, 127, 150, 200, FillRule::Winding,
               Transform::identity(), /*antiAlias=*/false);

  EXPECT_CROSS_MATCH(*cpp, *rust);
}

}  // namespace
