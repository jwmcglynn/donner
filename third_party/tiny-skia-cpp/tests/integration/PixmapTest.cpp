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

using namespace tiny_skia;

// ---------------------------------------------------------------------------
// clone_rect_1
// ---------------------------------------------------------------------------
TEST(PixmapTest, CloneRect1) {
    auto pixmap = Pixmap::fromSize(200, 200);
    ASSERT_TRUE(pixmap.has_value());

    Paint paint;
    paint.setColorRgba8(50, 127, 150, 200);
    paint.antiAlias = true;

    auto circle = Path::fromCircle(100.0f, 100.0f, 80.0f);
    ASSERT_TRUE(circle.has_value());

    auto mut = pixmap->mutableView();
    Painter::fillPath(mut, *circle, paint, FillRule::Winding, Transform::identity());

    auto rect = IntRect::fromXYWH(10, 15, 80, 90);
    ASSERT_TRUE(rect.has_value());
    auto part = pixmap->view().cloneRect(*rect);
    ASSERT_TRUE(part.has_value());

    EXPECT_GOLDEN_MATCH(*part, "pixmap/clone-rect-1.png");
}

// ---------------------------------------------------------------------------
// clone_rect_2
// ---------------------------------------------------------------------------
TEST(PixmapTest, CloneRect2) {
    auto pixmap = Pixmap::fromSize(200, 200);
    ASSERT_TRUE(pixmap.has_value());

    Paint paint;
    paint.setColorRgba8(50, 127, 150, 200);
    paint.antiAlias = true;

    auto circle = Path::fromCircle(100.0f, 100.0f, 80.0f);
    ASSERT_TRUE(circle.has_value());

    auto mut = pixmap->mutableView();
    Painter::fillPath(mut, *circle, paint, FillRule::Winding, Transform::identity());

    auto rect = IntRect::fromXYWH(130, 120, 80, 90);
    ASSERT_TRUE(rect.has_value());
    auto part = pixmap->view().cloneRect(*rect);
    ASSERT_TRUE(part.has_value());

    EXPECT_GOLDEN_MATCH(*part, "pixmap/clone-rect-2.png");
}

// ---------------------------------------------------------------------------
// clone_rect_out_of_bound
// ---------------------------------------------------------------------------
TEST(PixmapTest, CloneRectOutOfBound) {
    auto pixmap = Pixmap::fromSize(200, 200);
    ASSERT_TRUE(pixmap.has_value());

    Paint paint;
    paint.setColorRgba8(50, 127, 150, 200);
    paint.antiAlias = true;

    auto circle = Path::fromCircle(100.0f, 100.0f, 80.0f);
    ASSERT_TRUE(circle.has_value());

    auto mut = pixmap->mutableView();
    Painter::fillPath(mut, *circle, paint, FillRule::Winding, Transform::identity());

    // x=250 is out of bounds for a 200-wide pixmap
    auto rect1 = IntRect::fromXYWH(250, 15, 80, 90);
    ASSERT_TRUE(rect1.has_value());
    EXPECT_FALSE(pixmap->view().cloneRect(*rect1).has_value());

    // y=250 is out of bounds for a 200-tall pixmap
    auto rect2 = IntRect::fromXYWH(10, 250, 80, 90);
    ASSERT_TRUE(rect2.has_value());
    EXPECT_FALSE(pixmap->view().cloneRect(*rect2).has_value());

    // y=-250 is out of bounds (negative)
    auto rect3 = IntRect::fromXYWH(10, -250, 80, 90);
    ASSERT_TRUE(rect3.has_value());
    EXPECT_FALSE(pixmap->view().cloneRect(*rect3).has_value());
}

// ---------------------------------------------------------------------------
// fill
// ---------------------------------------------------------------------------
TEST(PixmapTest, fill) {
    auto c = Color::fromRgba8(50, 100, 150, 200);
    auto pixmap = Pixmap::fromSize(10, 10);
    ASSERT_TRUE(pixmap.has_value());

    pixmap->fill(c);

    auto pixel = pixmap->pixel(1, 1);
    ASSERT_TRUE(pixel.has_value());
    auto expected = c.premultiply().toColorU8();
    EXPECT_EQ(*pixel, expected);
}

// ---------------------------------------------------------------------------
// draw_pixmap
// Tests that painting algorithm will switch Bicubic/Bilinear to Nearest.
// Otherwise we will get a blurry image.
// ---------------------------------------------------------------------------
TEST(PixmapTest, DrawPixmap) {
    // A pixmap with the bottom half filled with solid color.
    auto subPixmap = Pixmap::fromSize(100, 100);
    ASSERT_TRUE(subPixmap.has_value());
    {
        Paint paint;
        paint.setColorRgba8(50, 127, 150, 200);
        paint.antiAlias = false;

        auto rect = Rect::fromLTRB(0.0f, 50.0f, 100.0f, 100.0f);
        ASSERT_TRUE(rect.has_value());

        auto mut = subPixmap->mutableView();
        Painter::fillRect(mut, *rect, paint, Transform::identity());
    }

    PixmapPaint ppaint;
    ppaint.quality = FilterQuality::Bicubic;

    auto pixmap = Pixmap::fromSize(200, 200);
    ASSERT_TRUE(pixmap.has_value());

    auto mut = pixmap->mutableView();
    Painter::drawPixmap(mut, 20, 20, subPixmap->view(), ppaint, Transform::identity());

    EXPECT_GOLDEN_MATCH(*pixmap, "canvas/draw-pixmap.png");
}

// ---------------------------------------------------------------------------
// draw_pixmap_ts
// ---------------------------------------------------------------------------
TEST(PixmapTest, DrawPixmapTs) {
    // Create a triangle sub-pixmap.
    auto triangle = Pixmap::fromSize(100, 100);
    ASSERT_TRUE(triangle.has_value());
    {
        Paint paint;
        paint.setColorRgba8(50, 127, 150, 200);
        paint.antiAlias = true;

        PathBuilder pb;
        pb.moveTo(0.0f, 100.0f);
        pb.lineTo(100.0f, 100.0f);
        pb.lineTo(50.0f, 0.0f);
        pb.close();
        auto path = pb.finish();
        ASSERT_TRUE(path.has_value());

        auto mut = triangle->mutableView();
        Painter::fillPath(mut, *path, paint, FillRule::Winding, Transform::identity());
    }

    PixmapPaint ppaint;
    ppaint.quality = FilterQuality::Bicubic;

    auto pixmap = Pixmap::fromSize(200, 200);
    ASSERT_TRUE(pixmap.has_value());

    auto mut = pixmap->mutableView();
    Painter::drawPixmap(mut, 5, 10, triangle->view(), ppaint,
               Transform::fromRow(1.2f, 0.5f, 0.5f, 1.2f, 0.0f, 0.0f));

    EXPECT_GOLDEN_MATCH(*pixmap, "canvas/draw-pixmap-ts.png");
}

// ---------------------------------------------------------------------------
// draw_pixmap_opacity
// ---------------------------------------------------------------------------
TEST(PixmapTest, DrawPixmapOpacity) {
    // Create a triangle sub-pixmap.
    auto triangle = Pixmap::fromSize(100, 100);
    ASSERT_TRUE(triangle.has_value());
    {
        Paint paint;
        paint.setColorRgba8(50, 127, 150, 200);
        paint.antiAlias = true;

        PathBuilder pb;
        pb.moveTo(0.0f, 100.0f);
        pb.lineTo(100.0f, 100.0f);
        pb.lineTo(50.0f, 0.0f);
        pb.close();
        auto path = pb.finish();
        ASSERT_TRUE(path.has_value());

        auto mut = triangle->mutableView();
        Painter::fillPath(mut, *path, paint, FillRule::Winding, Transform::identity());
    }

    PixmapPaint ppaint;
    ppaint.quality = FilterQuality::Bicubic;
    ppaint.opacity = 0.5f;

    auto pixmap = Pixmap::fromSize(200, 200);
    ASSERT_TRUE(pixmap.has_value());

    auto mut = pixmap->mutableView();
    Painter::drawPixmap(mut, 5, 10, triangle->view(), ppaint,
               Transform::fromRow(1.2f, 0.5f, 0.5f, 1.2f, 0.0f, 0.0f));

    EXPECT_GOLDEN_MATCH(*pixmap, "canvas/draw-pixmap-opacity.png");
}
