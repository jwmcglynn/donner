#include <gtest/gtest.h>
#include "ImageComparisonTestFixture.h"
#include "tiny_skia/Color.h"
#include "tiny_skia/Geom.h"
#include "tiny_skia/Painter.h"
#include "tiny_skia/Path.h"
#include "tiny_skia/PathBuilder.h"
#include "tiny_skia/Pixmap.h"
#include "tiny_skia/Stroke.h"
#include "tiny_skia/shaders/Shaders.h"
#include "tiny_skia/Mask.h"

using namespace tiny_skia;
using tiny_skia::test_utils::Params;

namespace {

TEST(DashTest, Line) {
    PathBuilder pb;
    pb.moveTo(10.0f, 20.0f);
    pb.lineTo(90.0f, 80.0f);
    auto path = pb.finish();
    ASSERT_TRUE(path.has_value());

    Paint paint;
    paint.setColorRgba8(50, 127, 150, 200);
    paint.antiAlias = false;

    Stroke stroke;
    stroke.dash = StrokeDash::create(std::vector<float>{5.0f, 10.0f}, 0.0f);
    stroke.width = 2.0f;

    auto pixmap = Pixmap::fromSize(100, 100);
    ASSERT_TRUE(pixmap.has_value());
    auto mut = pixmap->mutableView();
    Painter::strokePath(mut, *path, paint, stroke, Transform::identity());

    EXPECT_GOLDEN_MATCH(*pixmap, "dash/line.png");
}

TEST(DashTest, Quad) {
    PathBuilder pb;
    pb.moveTo(10.0f, 20.0f);
    pb.quadTo(35.0f, 75.0f, 90.0f, 80.0f);
    auto path = pb.finish();
    ASSERT_TRUE(path.has_value());

    Paint paint;
    paint.setColorRgba8(50, 127, 150, 200);
    paint.antiAlias = false;

    Stroke stroke;
    stroke.dash = StrokeDash::create(std::vector<float>{5.0f, 10.0f}, 0.0f);
    stroke.width = 2.0f;

    auto pixmap = Pixmap::fromSize(100, 100);
    ASSERT_TRUE(pixmap.has_value());
    auto mut = pixmap->mutableView();
    Painter::strokePath(mut, *path, paint, stroke, Transform::identity());

    EXPECT_GOLDEN_MATCH(*pixmap, "dash/quad.png");
}

TEST(DashTest, Cubic) {
    PathBuilder pb;
    pb.moveTo(10.0f, 20.0f);
    pb.cubicTo(95.0f, 35.0f, 0.0f, 75.0f, 75.0f, 90.0f);
    auto path = pb.finish();
    ASSERT_TRUE(path.has_value());

    Paint paint;
    paint.setColorRgba8(50, 127, 150, 200);
    paint.antiAlias = false;

    Stroke stroke;
    stroke.dash = StrokeDash::create(std::vector<float>{5.0f, 10.0f}, 0.0f);
    stroke.width = 2.0f;

    auto pixmap = Pixmap::fromSize(100, 100);
    ASSERT_TRUE(pixmap.has_value());
    auto mut = pixmap->mutableView();
    Painter::strokePath(mut, *path, paint, stroke, Transform::identity());

    EXPECT_GOLDEN_MATCH(*pixmap, "dash/cubic.png");
}

TEST(DashTest, Hairline) {
    PathBuilder pb;
    pb.moveTo(10.0f, 20.0f);
    pb.cubicTo(95.0f, 35.0f, 0.0f, 75.0f, 75.0f, 90.0f);
    auto path = pb.finish();
    ASSERT_TRUE(path.has_value());

    Paint paint;
    paint.setColorRgba8(50, 127, 150, 200);
    paint.antiAlias = true;

    Stroke stroke;
    stroke.dash = StrokeDash::create(std::vector<float>{5.0f, 10.0f}, 0.0f);
    stroke.width = 0.5f;

    auto pixmap = Pixmap::fromSize(100, 100);
    ASSERT_TRUE(pixmap.has_value());
    auto mut = pixmap->mutableView();
    Painter::strokePath(mut, *path, paint, stroke, Transform::identity());

    EXPECT_GOLDEN_MATCH(*pixmap, "dash/hairline.png");
}

TEST(DashTest, Complex) {
    PathBuilder pb;
    pb.moveTo(28.7f, 23.9f);
    pb.lineTo(177.4f, 35.2f);
    pb.lineTo(177.4f, 68.0f);
    pb.lineTo(129.7f, 68.0f);
    pb.cubicTo(81.6f, 59.3f, 41.8f, 63.3f, 33.4f, 115.2f);
    pb.cubicTo(56.8f, 128.7f, 77.3f, 143.8f, 53.3f, 183.8f);
    pb.cubicTo(113.8f, 185.7f, 91.0f, 109.7f, 167.3f, 111.8f);
    pb.cubicTo(-56.2f, 90.3f, 177.3f, 68.0f, 110.2f, 95.5f);
    auto path = pb.finish();
    ASSERT_TRUE(path.has_value());

    Paint paint;
    paint.setColorRgba8(50, 127, 150, 200);
    paint.antiAlias = true;

    Stroke stroke;
    stroke.dash = StrokeDash::create(std::vector<float>{10.0f, 5.0f}, 2.0f);
    stroke.width = 2.0f;

    auto pixmap = Pixmap::fromSize(200, 200);
    ASSERT_TRUE(pixmap.has_value());
    auto mut = pixmap->mutableView();
    Painter::strokePath(mut, *path, paint, stroke, Transform::identity());

    EXPECT_GOLDEN_MATCH_WITH_PARAMS(*pixmap, "dash/complex.png",
                                    Params::WithThreshold(0.1f));
}

TEST(DashTest, MultiSubpaths) {
    PathBuilder pb;
    pb.moveTo(49.0f, 76.0f);
    pb.cubicTo(22.0f, 150.0f, 11.0f, 213.0f, 186.0f, 151.0f);
    pb.cubicTo(194.0f, 106.0f, 195.0f, 64.0f, 169.0f, 26.0f);
    pb.moveTo(124.0f, 41.0f);
    pb.lineTo(162.0f, 105.0f);
    pb.cubicTo(135.0f, 175.0f, 97.0f, 166.0f, 53.0f, 128.0f);
    pb.lineTo(93.0f, 71.0f);
    pb.moveTo(24.0f, 52.0f);
    pb.lineTo(108.0f, 20.0f);
    auto path = pb.finish();
    ASSERT_TRUE(path.has_value());

    Paint paint;
    paint.setColorRgba8(50, 127, 150, 200);
    paint.antiAlias = true;

    Stroke stroke;
    stroke.dash = StrokeDash::create(std::vector<float>{10.0f, 5.0f}, 2.0f);
    stroke.width = 2.0f;

    auto pixmap = Pixmap::fromSize(200, 200);
    ASSERT_TRUE(pixmap.has_value());
    auto mut = pixmap->mutableView();
    Painter::strokePath(mut, *path, paint, stroke, Transform::identity());

    EXPECT_GOLDEN_MATCH_WITH_PARAMS(*pixmap, "dash/multi_subpaths.png",
                                    Params::WithThreshold(0.1f));
}

TEST(DashTest, Closed) {
    PathBuilder pb;
    pb.moveTo(22.0f, 22.0f);
    pb.cubicTo(63.0f, 16.0f, 82.0f, 24.0f, 84.0f, 46.0f);
    pb.cubicTo(86.0f, 73.0f, 15.0f, 58.0f, 16.0f, 89.0f);
    pb.close();
    auto path = pb.finish();
    ASSERT_TRUE(path.has_value());

    Paint paint;
    paint.setColorRgba8(50, 127, 150, 200);
    paint.antiAlias = true;

    Stroke stroke;
    stroke.dash = StrokeDash::create(std::vector<float>{10.0f, 5.0f}, 2.0f);
    stroke.width = 2.0f;

    auto pixmap = Pixmap::fromSize(100, 100);
    ASSERT_TRUE(pixmap.has_value());
    auto mut = pixmap->mutableView();
    Painter::strokePath(mut, *path, paint, stroke, Transform::identity());

    EXPECT_GOLDEN_MATCH_WITH_PARAMS(*pixmap, "dash/closed.png",
                                    Params::WithThreshold(0.1f));
}

}  // namespace
