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

TEST(StrokeTest, RoundCapsAndLargeScale) {
    Paint paint;
    paint.setColorRgba8(50, 127, 150, 200);
    paint.antiAlias = true;

    PathBuilder pb;
    pb.moveTo(60.0f / 16.0f, 100.0f / 16.0f);
    pb.lineTo(140.0f / 16.0f, 100.0f / 16.0f);
    auto path = pb.finish();
    ASSERT_TRUE(path.has_value());

    Stroke stroke;
    stroke.width = 6.0f;
    stroke.lineCap = LineCap::Round;

    auto ts = Transform::fromScale(16.0f, 16.0f);

    auto pixmap = Pixmap::fromSize(200, 200);
    ASSERT_TRUE(pixmap.has_value());
    auto mut = pixmap->mutableView();
    Painter::strokePath(mut, *path, paint, stroke, ts);

    EXPECT_GOLDEN_MATCH_WITH_PARAMS(*pixmap, "stroke/round-caps-and-large-scale.png",
                                    Params::WithThreshold(0.1f));
}

TEST(StrokeTest, Circle) {
    Paint paint;
    paint.setColorRgba8(50, 127, 150, 200);
    paint.antiAlias = true;

    PathBuilder pb;
    pb.pushCircle(100.0f, 100.0f, 50.0f);
    auto path = pb.finish();
    ASSERT_TRUE(path.has_value());

    Stroke stroke;
    stroke.width = 2.0f;

    auto pixmap = Pixmap::fromSize(200, 200);
    ASSERT_TRUE(pixmap.has_value());
    auto mut = pixmap->mutableView();
    Painter::strokePath(mut, *path, paint, stroke, Transform::identity());

    EXPECT_GOLDEN_MATCH_WITH_PARAMS(*pixmap, "stroke/circle.png",
                                    Params::WithThreshold(0.1f));
}

TEST(StrokeTest, ZeroLenSubpathButtCap) {
    Paint paint;
    paint.setColorRgba8(50, 127, 150, 200);
    paint.antiAlias = true;

    PathBuilder pb;
    pb.moveTo(50.0f, 50.0f);
    pb.lineTo(50.0f, 50.0f);
    auto path = pb.finish();
    ASSERT_TRUE(path.has_value());

    Stroke stroke;
    stroke.width = 20.0f;
    stroke.lineCap = LineCap::Butt;

    auto pixmap = Pixmap::fromSize(100, 100);
    ASSERT_TRUE(pixmap.has_value());
    auto mut = pixmap->mutableView();
    Painter::strokePath(mut, *path, paint, stroke, Transform::identity());

    EXPECT_GOLDEN_MATCH(*pixmap, "stroke/zero-len-subpath-butt-cap.png");
}

TEST(StrokeTest, ZeroLenSubpathRoundCap) {
    Paint paint;
    paint.setColorRgba8(50, 127, 150, 200);
    paint.antiAlias = true;

    PathBuilder pb;
    pb.moveTo(50.0f, 50.0f);
    pb.lineTo(50.0f, 50.0f);
    auto path = pb.finish();
    ASSERT_TRUE(path.has_value());

    Stroke stroke;
    stroke.width = 20.0f;
    stroke.lineCap = LineCap::Round;

    auto pixmap = Pixmap::fromSize(100, 100);
    ASSERT_TRUE(pixmap.has_value());
    auto mut = pixmap->mutableView();
    Painter::strokePath(mut, *path, paint, stroke, Transform::identity());

    EXPECT_GOLDEN_MATCH(*pixmap, "stroke/zero-len-subpath-round-cap.png");
}

TEST(StrokeTest, ZeroLenSubpathSquareCap) {
    Paint paint;
    paint.setColorRgba8(50, 127, 150, 200);
    paint.antiAlias = true;

    PathBuilder pb;
    pb.moveTo(50.0f, 50.0f);
    pb.lineTo(50.0f, 50.0f);
    auto path = pb.finish();
    ASSERT_TRUE(path.has_value());

    Stroke stroke;
    stroke.width = 20.0f;
    stroke.lineCap = LineCap::Square;

    auto pixmap = Pixmap::fromSize(100, 100);
    ASSERT_TRUE(pixmap.has_value());
    auto mut = pixmap->mutableView();
    Painter::strokePath(mut, *path, paint, stroke, Transform::identity());

    EXPECT_GOLDEN_MATCH(*pixmap, "stroke/zero-len-subpath-square-cap.png");
}

TEST(StrokeTest, RoundCapJoin) {
    Paint paint;
    paint.setColorRgba8(50, 127, 150, 200);
    paint.antiAlias = true;

    PathBuilder pb;
    pb.moveTo(170.0f, 30.0f);
    pb.lineTo(30.553378f, 99.048418f);
    pb.cubicTo(30.563658f, 99.066835f, 30.546308f, 99.280724f, 30.557592f, 99.305282f);
    auto path = pb.finish();
    ASSERT_TRUE(path.has_value());

    Stroke stroke;
    stroke.width = 30.0f;
    stroke.lineCap = LineCap::Round;
    stroke.lineJoin = LineJoin::Round;

    auto pixmap = Pixmap::fromSize(200, 200);
    ASSERT_TRUE(pixmap.has_value());
    auto mut = pixmap->mutableView();
    Painter::strokePath(mut, *path, paint, stroke, Transform::identity());

    EXPECT_GOLDEN_MATCH_WITH_PARAMS(*pixmap, "stroke/round-cap-join.png",
                                    Params::WithThreshold(0.1f));
}

}  // namespace
