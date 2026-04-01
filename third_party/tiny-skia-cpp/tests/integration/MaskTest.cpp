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

namespace {

TEST(MaskTest, Rect) {
    auto clipRect = Rect::fromLTRB(10.0f, 10.0f, 90.0f, 90.0f);
    ASSERT_TRUE(clipRect.has_value());
    auto clipPath = Path::fromRect(*clipRect);

    auto mask = Mask::fromSize(100, 100);
    ASSERT_TRUE(mask.has_value());
    mask->fillPath(clipPath, FillRule::Winding, false, Transform::identity());

    Paint paint;
    paint.setColorRgba8(50, 127, 150, 200);
    paint.antiAlias = false;

    auto pixmap = Pixmap::fromSize(100, 100);
    ASSERT_TRUE(pixmap.has_value());
    auto rect = Rect::fromLTRB(0.0f, 0.0f, 100.0f, 100.0f);
    ASSERT_TRUE(rect.has_value());
    auto mut = pixmap->mutableView();
    Painter::fillRect(mut, *rect, paint, Transform::identity(), &*mask);

    EXPECT_GOLDEN_MATCH(*pixmap, "mask/rect.png");
}

TEST(MaskTest, RectAa) {
    auto clipRect = Rect::fromLTRB(10.5f, 10.0f, 90.5f, 90.5f);
    ASSERT_TRUE(clipRect.has_value());
    auto clipPath = Path::fromRect(*clipRect);

    auto mask = Mask::fromSize(100, 100);
    ASSERT_TRUE(mask.has_value());
    mask->fillPath(clipPath, FillRule::Winding, true, Transform::identity());

    Paint paint;
    paint.setColorRgba8(50, 127, 150, 200);
    paint.antiAlias = false;

    auto pixmap = Pixmap::fromSize(100, 100);
    ASSERT_TRUE(pixmap.has_value());
    auto rect = Rect::fromLTRB(0.0f, 0.0f, 100.0f, 100.0f);
    ASSERT_TRUE(rect.has_value());
    auto mut = pixmap->mutableView();
    Painter::fillRect(mut, *rect, paint, Transform::identity(), &*mask);

    EXPECT_GOLDEN_MATCH(*pixmap, "mask/rect-aa.png");
}

TEST(MaskTest, RectTs) {
    auto pixmap = Pixmap::fromSize(100, 100);
    ASSERT_TRUE(pixmap.has_value());

    auto clipRect = Rect::fromLTRB(10.0f, 10.0f, 90.0f, 90.0f);
    ASSERT_TRUE(clipRect.has_value());
    auto clipPath = Path::fromRect(*clipRect);
    auto transformedPath = clipPath.transform(Transform::fromRow(1.0f, -0.3f, 0.0f, 1.0f, 0.0f, 15.0f));
    ASSERT_TRUE(transformedPath.has_value());

    auto mask = Mask::fromSize(100, 100);
    ASSERT_TRUE(mask.has_value());
    mask->fillPath(*transformedPath, FillRule::Winding, false, Transform::identity());

    Paint paint;
    paint.setColorRgba8(50, 127, 150, 200);
    paint.antiAlias = false;

    auto rect = Rect::fromLTRB(0.0f, 0.0f, 100.0f, 100.0f);
    ASSERT_TRUE(rect.has_value());
    auto mut = pixmap->mutableView();
    Painter::fillRect(mut, *rect, paint, Transform::identity(), &*mask);

    EXPECT_GOLDEN_MATCH(*pixmap, "mask/rect-ts.png");
}

TEST(MaskTest, CircleBottomRightAa) {
    auto pixmap = Pixmap::fromSize(100, 100);
    ASSERT_TRUE(pixmap.has_value());

    PathBuilder pb;
    pb.pushCircle(100.0f, 100.0f, 50.0f);
    auto clipPath = pb.finish();
    ASSERT_TRUE(clipPath.has_value());

    auto mask = Mask::fromSize(100, 100);
    ASSERT_TRUE(mask.has_value());
    mask->fillPath(*clipPath, FillRule::Winding, true, Transform::identity());

    Paint paint;
    paint.setColorRgba8(50, 127, 150, 200);
    paint.antiAlias = false;

    auto rect = Rect::fromLTRB(0.0f, 0.0f, 100.0f, 100.0f);
    ASSERT_TRUE(rect.has_value());
    auto mut = pixmap->mutableView();
    Painter::fillRect(mut, *rect, paint, Transform::identity(), &*mask);

    EXPECT_GOLDEN_MATCH(*pixmap, "mask/circle-bottom-right-aa.png");
}

TEST(MaskTest, Stroke) {
    auto pixmap = Pixmap::fromSize(100, 100);
    ASSERT_TRUE(pixmap.has_value());

    auto clipRect = Rect::fromLTRB(10.0f, 10.0f, 90.0f, 90.0f);
    ASSERT_TRUE(clipRect.has_value());
    auto clipPath = Path::fromRect(*clipRect);

    auto mask = Mask::fromSize(100, 100);
    ASSERT_TRUE(mask.has_value());
    mask->fillPath(clipPath, FillRule::Winding, false, Transform::identity());

    Paint paint;
    paint.setColorRgba8(50, 127, 150, 200);
    paint.antiAlias = false;

    tiny_skia::Stroke stroke;
    stroke.width = 10.0f;

    auto pathRect = Rect::fromLTRB(10.0f, 10.0f, 90.0f, 90.0f);
    ASSERT_TRUE(pathRect.has_value());
    auto path = Path::fromRect(*pathRect);

    auto mut = pixmap->mutableView();
    Painter::strokePath(mut, path, paint, stroke, Transform::identity(), &*mask);

    EXPECT_GOLDEN_MATCH(*pixmap, "mask/stroke.png");
}

TEST(MaskTest, SkipDest) {
    auto pixmap = Pixmap::fromSize(100, 100);
    ASSERT_TRUE(pixmap.has_value());

    Paint paint;
    paint.setColorRgba8(50, 127, 150, 200);
    paint.antiAlias = false;

    // Draw first rect on pixmap
    auto rectPath1 = Rect::fromLTRB(5.0f, 5.0f, 65.0f, 65.0f);
    ASSERT_TRUE(rectPath1.has_value());
    auto path1 = Path::fromRect(*rectPath1);
    {
        auto mut = pixmap->mutableView();
        Painter::fillPath(mut, path1, paint, FillRule::Winding, Transform::identity());
    }

    // Draw second rect on pixmap2
    auto pixmap2 = Pixmap::fromSize(200, 200);
    ASSERT_TRUE(pixmap2.has_value());
    auto rectPath2 = Rect::fromLTRB(35.0f, 35.0f, 95.0f, 95.0f);
    ASSERT_TRUE(rectPath2.has_value());
    auto path2 = Path::fromRect(*rectPath2);
    {
        auto mut2 = pixmap2->mutableView();
        Painter::fillPath(mut2, path2, paint, FillRule::Winding, Transform::identity());
    }

    // Create mask
    auto clipRect = Rect::fromLTRB(40.0f, 40.0f, 80.0f, 80.0f);
    ASSERT_TRUE(clipRect.has_value());
    auto clipPath = Path::fromRect(*clipRect);
    auto mask = Mask::fromSize(100, 100);
    ASSERT_TRUE(mask.has_value());
    mask->fillPath(clipPath, FillRule::Winding, true, Transform::identity());

    // Draw pixmap2 onto pixmap with mask
    PixmapPaint ppaint;
    auto mut = pixmap->mutableView();
    Painter::drawPixmap(mut, 0, 0, pixmap2->view(), ppaint, Transform::identity(), &*mask);

    EXPECT_GOLDEN_MATCH(*pixmap, "mask/skip-dest.png");
}

TEST(MaskTest, IntersectAa) {
    PathBuilder pb1;
    pb1.pushCircle(75.0f, 75.0f, 50.0f);
    auto circle1 = pb1.finish();
    ASSERT_TRUE(circle1.has_value());

    PathBuilder pb2;
    pb2.pushCircle(125.0f, 125.0f, 50.0f);
    auto circle2 = pb2.finish();
    ASSERT_TRUE(circle2.has_value());

    auto mask = Mask::fromSize(200, 200);
    ASSERT_TRUE(mask.has_value());
    mask->fillPath(*circle1, FillRule::Winding, true, Transform::identity());
    mask->intersectPath(*circle2, FillRule::Winding, true, Transform::identity());

    Paint paint;
    paint.setColorRgba8(50, 127, 150, 200);
    paint.antiAlias = false;

    auto pixmap = Pixmap::fromSize(200, 200);
    ASSERT_TRUE(pixmap.has_value());
    auto rect = Rect::fromLTRB(0.0f, 0.0f, 200.0f, 200.0f);
    ASSERT_TRUE(rect.has_value());
    auto mut = pixmap->mutableView();
    Painter::fillRect(mut, *rect, paint, Transform::identity(), &*mask);

    EXPECT_GOLDEN_MATCH(*pixmap, "mask/intersect-aa.png");
}

TEST(MaskTest, IgnoreMemset) {
    auto clipRect = Rect::fromLTRB(10.0f, 10.0f, 90.0f, 90.0f);
    ASSERT_TRUE(clipRect.has_value());
    auto clipPath = Path::fromRect(*clipRect);

    auto mask = Mask::fromSize(100, 100);
    ASSERT_TRUE(mask.has_value());
    mask->fillPath(clipPath, FillRule::Winding, false, Transform::identity());

    Paint paint;
    paint.setColorRgba8(50, 127, 150, 255);
    paint.antiAlias = false;

    auto pixmap = Pixmap::fromSize(100, 100);
    ASSERT_TRUE(pixmap.has_value());
    auto rect = Rect::fromLTRB(0.0f, 0.0f, 100.0f, 100.0f);
    ASSERT_TRUE(rect.has_value());
    auto mut = pixmap->mutableView();
    Painter::fillRect(mut, *rect, paint, Transform::identity(), &*mask);

    EXPECT_GOLDEN_MATCH(*pixmap, "mask/ignore-memset.png");
}

TEST(MaskTest, IgnoreSource) {
    auto clipRect = Rect::fromLTRB(10.0f, 10.0f, 90.0f, 90.0f);
    ASSERT_TRUE(clipRect.has_value());
    auto clipPath = Path::fromRect(*clipRect);

    auto mask = Mask::fromSize(100, 100);
    ASSERT_TRUE(mask.has_value());
    mask->fillPath(clipPath, FillRule::Winding, false, Transform::identity());

    Paint paint;
    paint.setColorRgba8(50, 127, 150, 255);  // Must be opaque.
    paint.blendMode = BlendMode::SourceOver;
    paint.antiAlias = false;

    auto pixmap = Pixmap::fromSize(100, 100);
    ASSERT_TRUE(pixmap.has_value());
    pixmap->fill(Color::white);

    auto rect = Rect::fromLTRB(0.0f, 0.0f, 100.0f, 100.0f);
    ASSERT_TRUE(rect.has_value());
    auto mut = pixmap->mutableView();
    Painter::fillRect(mut, *rect, paint, Transform::identity(), &*mask);

    EXPECT_GOLDEN_MATCH(*pixmap, "mask/ignore-source.png");
}

TEST(MaskTest, ApplyMask) {
    auto pixmap = Pixmap::fromSize(100, 100);
    ASSERT_TRUE(pixmap.has_value());

    PathBuilder pb;
    pb.pushCircle(100.0f, 100.0f, 50.0f);
    auto clipPath = pb.finish();
    ASSERT_TRUE(clipPath.has_value());

    auto mask = Mask::fromSize(100, 100);
    ASSERT_TRUE(mask.has_value());
    mask->fillPath(*clipPath, FillRule::Winding, true, Transform::identity());

    Paint paint;
    paint.setColorRgba8(50, 127, 150, 200);
    paint.antiAlias = false;

    auto rect = Rect::fromLTRB(0.0f, 0.0f, 100.0f, 100.0f);
    ASSERT_TRUE(rect.has_value());
    {
        auto mut = pixmap->mutableView();
        Painter::fillRect(mut, *rect, paint, Transform::identity());
    }
    {
        auto mut = pixmap->mutableView();
        Painter::applyMask(mut, *mask);
    }

    EXPECT_GOLDEN_MATCH(*pixmap, "mask/apply-mask.png");
}

}  // namespace
