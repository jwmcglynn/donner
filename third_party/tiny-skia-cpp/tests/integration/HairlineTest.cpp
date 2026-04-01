#include <gtest/gtest.h>
#include "ImageComparisonTestFixture.h"
#include "tiny_skia/Color.h"
#include "tiny_skia/Geom.h"
#include "tiny_skia/Painter.h"
#include "tiny_skia/Path.h"
#include "tiny_skia/PathBuilder.h"
#include "tiny_skia/Pixmap.h"
#include "tiny_skia/Stroke.h"

using namespace tiny_skia;

namespace {

// ---------------------------------------------------------------------------
// Helper: draw a single line segment and return the resulting pixmap.
// Mirrors the Rust `draw_line` helper.
// ---------------------------------------------------------------------------
Pixmap drawLine(float x0, float y0, float x1, float y1,
                bool antiAlias, float width, LineCap lineCap) {
    auto pixmap = Pixmap::fromSize(100, 100);
    EXPECT_TRUE(pixmap.has_value());

    PathBuilder pb;
    pb.moveTo(x0, y0);
    pb.lineTo(x1, y1);
    auto path = pb.finish();
    EXPECT_TRUE(path.has_value());

    Paint paint;
    paint.setColorRgba8(50, 127, 150, 200);
    paint.antiAlias = antiAlias;

    Stroke stroke;
    stroke.width = width;
    stroke.lineCap = lineCap;

    auto mut = pixmap->mutableView();
    Painter::strokePath(mut, *path, paint, stroke, Transform::identity());

    return std::move(*pixmap);
}

// ---------------------------------------------------------------------------
// Helper: draw a single quadratic curve and return the resulting pixmap.
// Mirrors the Rust `draw_quad` helper.
// ---------------------------------------------------------------------------
Pixmap drawQuad(bool antiAlias, float width, LineCap lineCap) {
    auto pixmap = Pixmap::fromSize(200, 100);
    EXPECT_TRUE(pixmap.has_value());

    PathBuilder pb;
    pb.moveTo(25.0f, 80.0f);
    pb.quadTo(155.0f, 75.0f, 175.0f, 20.0f);
    auto path = pb.finish();
    EXPECT_TRUE(path.has_value());

    Paint paint;
    paint.setColorRgba8(50, 127, 150, 200);
    paint.antiAlias = antiAlias;

    Stroke stroke;
    stroke.width = width;
    stroke.lineCap = lineCap;

    auto mut = pixmap->mutableView();
    Painter::strokePath(mut, *path, paint, stroke, Transform::identity());

    return std::move(*pixmap);
}

// ---------------------------------------------------------------------------
// Helper: draw a single cubic curve and return the resulting pixmap.
// Mirrors the Rust `draw_cubic` helper.
// ---------------------------------------------------------------------------
Pixmap drawCubic(const float (&points)[8], bool antiAlias, float width,
                 LineCap lineCap) {
    auto pixmap = Pixmap::fromSize(200, 100);
    EXPECT_TRUE(pixmap.has_value());

    PathBuilder pb;
    pb.moveTo(points[0], points[1]);
    pb.cubicTo(points[2], points[3], points[4], points[5],
               points[6], points[7]);
    auto path = pb.finish();
    EXPECT_TRUE(path.has_value());

    Paint paint;
    paint.setColorRgba8(50, 127, 150, 200);
    paint.antiAlias = antiAlias;

    Stroke stroke;
    stroke.width = width;
    stroke.lineCap = lineCap;

    auto mut = pixmap->mutableView();
    Painter::strokePath(mut, *path, paint, stroke, Transform::identity());

    return std::move(*pixmap);
}

}  // namespace

// ===========================================================================
// Line tests
// ===========================================================================

TEST(Hairline, Hline05) {
    auto pixmap = drawLine(10.0f, 10.0f, 90.0f, 10.0f,
                           false, 0.5f, LineCap::Butt);
    EXPECT_GOLDEN_MATCH(pixmap, "hairline/hline-05.png");
}

TEST(Hairline, Hline05Aa) {
    auto pixmap = drawLine(10.0f, 10.0f, 90.0f, 10.0f,
                           true, 0.5f, LineCap::Butt);
    EXPECT_GOLDEN_MATCH(pixmap, "hairline/hline-05-aa.png");
}

TEST(Hairline, Hline05AaRound) {
    auto pixmap = drawLine(10.0f, 10.0f, 90.0f, 10.0f,
                           true, 0.5f, LineCap::Round);
    EXPECT_GOLDEN_MATCH(pixmap, "hairline/hline-05-aa-round.png");
}

TEST(Hairline, Vline05) {
    auto pixmap = drawLine(10.0f, 10.0f, 10.0f, 90.0f,
                           false, 0.5f, LineCap::Butt);
    EXPECT_GOLDEN_MATCH(pixmap, "hairline/vline-05.png");
}

TEST(Hairline, Vline05Aa) {
    auto pixmap = drawLine(10.0f, 10.0f, 10.0f, 90.0f,
                           true, 0.5f, LineCap::Butt);
    EXPECT_GOLDEN_MATCH(pixmap, "hairline/vline-05-aa.png");
}

TEST(Hairline, Vline05AaRound) {
    auto pixmap = drawLine(10.0f, 10.0f, 10.0f, 90.0f,
                           true, 0.5f, LineCap::Round);
    EXPECT_GOLDEN_MATCH(pixmap, "hairline/vline-05-aa-round.png");
}

TEST(Hairline, Horish05Aa) {
    auto pixmap = drawLine(10.0f, 10.0f, 90.0f, 70.0f,
                           true, 0.5f, LineCap::Butt);
    EXPECT_GOLDEN_MATCH(pixmap, "hairline/horish-05-aa.png");
}

TEST(Hairline, Vertish05Aa) {
    auto pixmap = drawLine(10.0f, 10.0f, 70.0f, 90.0f,
                           true, 0.5f, LineCap::Butt);
    EXPECT_GOLDEN_MATCH(pixmap, "hairline/vertish-05-aa.png");
}

TEST(Hairline, ClipLine05Aa) {
    auto pixmap = drawLine(-10.0f, 10.0f, 110.0f, 70.0f,
                           true, 0.5f, LineCap::Butt);
    EXPECT_GOLDEN_MATCH(pixmap, "hairline/clip-line-05-aa.png");
}

TEST(Hairline, ClipLine00) {
    auto pixmap = drawLine(-10.0f, 10.0f, 110.0f, 70.0f,
                           false, 0.0f, LineCap::Butt);
    EXPECT_GOLDEN_MATCH(pixmap, "hairline/clip-line-00.png");
}

TEST(Hairline, ClipLine00V2) {
    auto pixmap = Pixmap::fromSize(512, 512);
    ASSERT_TRUE(pixmap.has_value());

    Paint paint;
    paint.setColorRgba8(50, 127, 150, 200);
    paint.antiAlias = false;

    Stroke stroke;
    stroke.width = 0.0f;

    PathBuilder builder;
    builder.moveTo(369.26462f, 577.8069f);
    builder.lineTo(488.0846f, 471.04388f);
    auto path = builder.finish();
    ASSERT_TRUE(path.has_value());

    auto mut = pixmap->mutableView();
    Painter::strokePath(mut, *path, paint, stroke, Transform::identity());

    EXPECT_GOLDEN_MATCH(*pixmap, "hairline/clip-line-00-v2.png");
}

TEST(Hairline, ClipHlineTopAa) {
    auto pixmap = drawLine(-1.0f, 0.0f, 101.0f, 0.0f,
                           true, 1.0f, LineCap::Butt);
    EXPECT_GOLDEN_MATCH(pixmap, "hairline/clip-hline-top-aa.png");
}

TEST(Hairline, ClipHlineBottomAa) {
    auto pixmap = drawLine(-1.0f, 100.0f, 101.0f, 100.0f,
                           true, 1.0f, LineCap::Butt);
    EXPECT_GOLDEN_MATCH(pixmap, "hairline/clip-hline-bottom-aa.png");
}

TEST(Hairline, ClipVlineLeftAa) {
    auto pixmap = drawLine(0.0f, -1.0f, 0.0f, 101.0f,
                           true, 1.0f, LineCap::Butt);
    EXPECT_GOLDEN_MATCH(pixmap, "hairline/clip-vline-left-aa.png");
}

TEST(Hairline, ClipVlineRightAa) {
    auto pixmap = drawLine(100.0f, -1.0f, 100.0f, 101.0f,
                           true, 1.0f, LineCap::Butt);
    EXPECT_GOLDEN_MATCH(pixmap, "hairline/clip-vline-right-aa.png");
}

// ===========================================================================
// Quad tests
// ===========================================================================

TEST(Hairline, QuadWidth05Aa) {
    auto pixmap = drawQuad(true, 0.5f, LineCap::Butt);
    EXPECT_GOLDEN_MATCH(pixmap, "hairline/quad-width-05-aa.png");
}

TEST(Hairline, QuadWidth05AaRound) {
    auto pixmap = drawQuad(true, 0.5f, LineCap::Round);
    EXPECT_GOLDEN_MATCH(pixmap, "hairline/quad-width-05-aa-round.png");
}

TEST(Hairline, QuadWidth00) {
    auto pixmap = drawQuad(false, 0.0f, LineCap::Butt);
    EXPECT_GOLDEN_MATCH(pixmap, "hairline/quad-width-00.png");
}

// ===========================================================================
// Cubic tests
// ===========================================================================

TEST(Hairline, CubicWidth10Aa) {
    const float pts[] = {25.0f, 80.0f, 55.0f, 25.0f,
                         155.0f, 75.0f, 175.0f, 20.0f};
    auto pixmap = drawCubic(pts, true, 1.0f, LineCap::Butt);
    EXPECT_GOLDEN_MATCH(pixmap, "hairline/cubic-width-10-aa.png");
}

TEST(Hairline, CubicWidth05Aa) {
    const float pts[] = {25.0f, 80.0f, 55.0f, 25.0f,
                         155.0f, 75.0f, 175.0f, 20.0f};
    auto pixmap = drawCubic(pts, true, 0.5f, LineCap::Butt);
    EXPECT_GOLDEN_MATCH(pixmap, "hairline/cubic-width-05-aa.png");
}

TEST(Hairline, CubicWidth00Aa) {
    const float pts[] = {25.0f, 80.0f, 55.0f, 25.0f,
                         155.0f, 75.0f, 175.0f, 20.0f};
    auto pixmap = drawCubic(pts, true, 0.0f, LineCap::Butt);
    EXPECT_GOLDEN_MATCH(pixmap, "hairline/cubic-width-00-aa.png");
}

TEST(Hairline, CubicWidth00) {
    const float pts[] = {25.0f, 80.0f, 55.0f, 25.0f,
                         155.0f, 75.0f, 175.0f, 20.0f};
    auto pixmap = drawCubic(pts, false, 0.0f, LineCap::Butt);
    EXPECT_GOLDEN_MATCH(pixmap, "hairline/cubic-width-00.png");
}

TEST(Hairline, CubicWidth05AaRound) {
    const float pts[] = {25.0f, 80.0f, 55.0f, 25.0f,
                         155.0f, 75.0f, 175.0f, 20.0f};
    auto pixmap = drawCubic(pts, true, 0.5f, LineCap::Round);
    EXPECT_GOLDEN_MATCH(pixmap, "hairline/cubic-width-05-aa-round.png");
}

TEST(Hairline, CubicWidth00Round) {
    const float pts[] = {25.0f, 80.0f, 55.0f, 25.0f,
                         155.0f, 75.0f, 175.0f, 20.0f};
    auto pixmap = drawCubic(pts, false, 0.0f, LineCap::Round);
    EXPECT_GOLDEN_MATCH(pixmap, "hairline/cubic-width-00-round.png");
}

TEST(Hairline, ChopCubic01) {
    // This curve invokes the chop_cubic_at_max_curvature branch of hair_cubic.
    const float pts[] = {57.0f, 13.0f, 17.0f, 15.0f,
                         55.0f, 97.0f, 89.0f, 62.0f};
    auto pixmap = drawCubic(pts, true, 0.5f, LineCap::Butt);
    EXPECT_GOLDEN_MATCH(pixmap, "hairline/chop-cubic-01.png");
}

TEST(Hairline, ClipCubic05Aa) {
    const float pts[] = {-25.0f, 80.0f, 55.0f, 25.0f,
                         155.0f, 75.0f, 175.0f, 20.0f};
    auto pixmap = drawCubic(pts, true, 0.5f, LineCap::Butt);
    EXPECT_GOLDEN_MATCH(pixmap, "hairline/clip-cubic-05-aa.png");
}

TEST(Hairline, ClipCubic00) {
    const float pts[] = {-25.0f, 80.0f, 55.0f, 25.0f,
                         155.0f, 75.0f, 175.0f, 20.0f};
    auto pixmap = drawCubic(pts, false, 0.0f, LineCap::Butt);
    EXPECT_GOLDEN_MATCH(pixmap, "hairline/clip-cubic-00.png");
}

// ===========================================================================
// Circle test
// ===========================================================================

TEST(Hairline, ClippedCircleAa) {
    auto pixmap = Pixmap::fromSize(100, 100);
    ASSERT_TRUE(pixmap.has_value());

    Paint paint;
    paint.setColorRgba8(50, 127, 150, 200);
    paint.antiAlias = true;

    Stroke stroke;
    stroke.width = 0.5f;

    auto path = Path::fromCircle(50.0f, 50.0f, 55.0f);
    ASSERT_TRUE(path.has_value());

    auto mut = pixmap->mutableView();
    Painter::strokePath(mut, *path, paint, stroke, Transform::identity());

    EXPECT_GOLDEN_MATCH(*pixmap, "hairline/clipped-circle-aa.png");
}
