#include <gtest/gtest.h>

#include <variant>
#include <vector>

#include "GoldenTestHelper.h"
#include "tiny_skia/Color.h"
#include "tiny_skia/Geom.h"
#include "tiny_skia/Painter.h"
#include "tiny_skia/Path.h"
#include "tiny_skia/Pixmap.h"
#include "tiny_skia/shaders/Shaders.h"

using namespace tiny_skia;

// ============================================================================
// Linear gradient tests - low quality pipeline
// ============================================================================

TEST(GradientsTest, TwoStopsLinearPadLq) {
    Paint paint;
    paint.antiAlias = false;

    auto result = LinearGradient::create(
        Point::fromXY(10.0f, 10.0f),
        Point::fromXY(190.0f, 190.0f),
        {
            GradientStop::create(0.0f, Color::fromRgba8(50, 127, 150, 200)),
            GradientStop::create(1.0f, Color::fromRgba8(220, 140, 75, 180)),
        },
        SpreadMode::Pad,
        Transform::identity());
    ASSERT_TRUE(result.has_value());
    paint.shader = std::get<LinearGradient>(std::move(*result));

    auto rect = Rect::fromLTRB(10.0f, 10.0f, 190.0f, 190.0f);
    auto path = Path::fromRect(*rect);

    auto pixmap = Pixmap::fromSize(200, 200);
    ASSERT_TRUE(pixmap.has_value());
    auto mut = pixmap->mutableView();
    Painter::fillPath(mut, path, paint, FillRule::Winding, Transform::identity());

    EXPECT_GOLDEN_MATCH(*pixmap, "gradients/two-stops-linear-pad-lq.png");
}

TEST(GradientsTest, TwoStopsLinearRepeatLq) {
    Paint paint;
    paint.antiAlias = false;

    auto result = LinearGradient::create(
        Point::fromXY(10.0f, 10.0f),
        Point::fromXY(100.0f, 100.0f),
        {
            GradientStop::create(0.0f, Color::fromRgba8(50, 127, 150, 200)),
            GradientStop::create(1.0f, Color::fromRgba8(220, 140, 75, 180)),
        },
        SpreadMode::Repeat,
        Transform::identity());
    ASSERT_TRUE(result.has_value());
    paint.shader = std::get<LinearGradient>(std::move(*result));

    auto rect = Rect::fromLTRB(10.0f, 10.0f, 190.0f, 190.0f);
    auto path = Path::fromRect(*rect);

    auto pixmap = Pixmap::fromSize(200, 200);
    ASSERT_TRUE(pixmap.has_value());
    auto mut = pixmap->mutableView();
    Painter::fillPath(mut, path, paint, FillRule::Winding, Transform::identity());

    EXPECT_GOLDEN_MATCH(*pixmap, "gradients/two-stops-linear-repeat-lq.png");
}

TEST(GradientsTest, TwoStopsLinearReflectLq) {
    Paint paint;
    paint.antiAlias = false;

    auto result = LinearGradient::create(
        Point::fromXY(10.0f, 10.0f),
        Point::fromXY(100.0f, 100.0f),
        {
            GradientStop::create(0.0f, Color::fromRgba8(50, 127, 150, 200)),
            GradientStop::create(1.0f, Color::fromRgba8(220, 140, 75, 180)),
        },
        SpreadMode::Reflect,
        Transform::identity());
    ASSERT_TRUE(result.has_value());
    paint.shader = std::get<LinearGradient>(std::move(*result));

    auto rect = Rect::fromLTRB(10.0f, 10.0f, 190.0f, 190.0f);
    auto path = Path::fromRect(*rect);

    auto pixmap = Pixmap::fromSize(200, 200);
    ASSERT_TRUE(pixmap.has_value());
    auto mut = pixmap->mutableView();
    Painter::fillPath(mut, path, paint, FillRule::Winding, Transform::identity());

    EXPECT_GOLDEN_MATCH(*pixmap, "gradients/two-stops-linear-reflect-lq.png");
}

TEST(GradientsTest, ThreeStopsEvenlySpacedLq) {
    Paint paint;
    paint.antiAlias = false;

    auto result = LinearGradient::create(
        Point::fromXY(10.0f, 10.0f),
        Point::fromXY(190.0f, 190.0f),
        {
            GradientStop::create(0.25f, Color::fromRgba8(50, 127, 150, 200)),
            GradientStop::create(0.50f, Color::fromRgba8(220, 140, 75, 180)),
            GradientStop::create(0.75f, Color::fromRgba8(40, 180, 55, 160)),
        },
        SpreadMode::Pad,
        Transform::identity());
    ASSERT_TRUE(result.has_value());
    paint.shader = std::get<LinearGradient>(std::move(*result));

    auto rect = Rect::fromLTRB(10.0f, 10.0f, 190.0f, 190.0f);
    auto path = Path::fromRect(*rect);

    auto pixmap = Pixmap::fromSize(200, 200);
    ASSERT_TRUE(pixmap.has_value());
    auto mut = pixmap->mutableView();
    Painter::fillPath(mut, path, paint, FillRule::Winding, Transform::identity());

    EXPECT_GOLDEN_MATCH(*pixmap, "gradients/three-stops-evenly-spaced-lq.png");
}

TEST(GradientsTest, TwoStopsUnevenlySpacedLq) {
    Paint paint;
    paint.antiAlias = false;

    auto result = LinearGradient::create(
        Point::fromXY(10.0f, 10.0f),
        Point::fromXY(190.0f, 190.0f),
        {
            GradientStop::create(0.25f, Color::fromRgba8(50, 127, 150, 200)),
            GradientStop::create(0.75f, Color::fromRgba8(220, 140, 75, 180)),
        },
        SpreadMode::Pad,
        Transform::identity());
    ASSERT_TRUE(result.has_value());
    paint.shader = std::get<LinearGradient>(std::move(*result));

    auto rect = Rect::fromLTRB(10.0f, 10.0f, 190.0f, 190.0f);
    auto path = Path::fromRect(*rect);

    auto pixmap = Pixmap::fromSize(200, 200);
    ASSERT_TRUE(pixmap.has_value());
    auto mut = pixmap->mutableView();
    Painter::fillPath(mut, path, paint, FillRule::Winding, Transform::identity());

    EXPECT_GOLDEN_MATCH(*pixmap, "gradients/two-stops-unevenly-spaced-lq.png");
}

// ============================================================================
// Linear gradient tests - high quality pipeline
// ============================================================================

TEST(GradientsTest, TwoStopsLinearPadHq) {
    Paint paint;
    paint.forceHqPipeline = true;
    paint.antiAlias = false;

    auto result = LinearGradient::create(
        Point::fromXY(10.0f, 10.0f),
        Point::fromXY(190.0f, 190.0f),
        {
            GradientStop::create(0.0f, Color::fromRgba8(50, 127, 150, 200)),
            GradientStop::create(1.0f, Color::fromRgba8(220, 140, 75, 180)),
        },
        SpreadMode::Pad,
        Transform::identity());
    ASSERT_TRUE(result.has_value());
    paint.shader = std::get<LinearGradient>(std::move(*result));

    auto rect = Rect::fromLTRB(10.0f, 10.0f, 190.0f, 190.0f);
    auto path = Path::fromRect(*rect);

    auto pixmap = Pixmap::fromSize(200, 200);
    ASSERT_TRUE(pixmap.has_value());
    auto mut = pixmap->mutableView();
    Painter::fillPath(mut, path, paint, FillRule::Winding, Transform::identity());

    EXPECT_GOLDEN_MATCH(*pixmap, "gradients/two-stops-linear-pad-hq.png");
}

TEST(GradientsTest, TwoStopsLinearRepeatHq) {
    Paint paint;
    paint.forceHqPipeline = true;
    paint.antiAlias = false;

    auto result = LinearGradient::create(
        Point::fromXY(10.0f, 10.0f),
        Point::fromXY(100.0f, 100.0f),
        {
            GradientStop::create(0.0f, Color::fromRgba8(50, 127, 150, 200)),
            GradientStop::create(1.0f, Color::fromRgba8(220, 140, 75, 180)),
        },
        SpreadMode::Repeat,
        Transform::identity());
    ASSERT_TRUE(result.has_value());
    paint.shader = std::get<LinearGradient>(std::move(*result));

    auto rect = Rect::fromLTRB(10.0f, 10.0f, 190.0f, 190.0f);
    auto path = Path::fromRect(*rect);

    auto pixmap = Pixmap::fromSize(200, 200);
    ASSERT_TRUE(pixmap.has_value());
    auto mut = pixmap->mutableView();
    Painter::fillPath(mut, path, paint, FillRule::Winding, Transform::identity());

    EXPECT_GOLDEN_MATCH(*pixmap, "gradients/two-stops-linear-repeat-hq.png");
}

TEST(GradientsTest, TwoStopsLinearReflectHq) {
    Paint paint;
    paint.forceHqPipeline = true;
    paint.antiAlias = false;

    auto result = LinearGradient::create(
        Point::fromXY(10.0f, 10.0f),
        Point::fromXY(100.0f, 100.0f),
        {
            GradientStop::create(0.0f, Color::fromRgba8(50, 127, 150, 200)),
            GradientStop::create(1.0f, Color::fromRgba8(220, 140, 75, 180)),
        },
        SpreadMode::Reflect,
        Transform::identity());
    ASSERT_TRUE(result.has_value());
    paint.shader = std::get<LinearGradient>(std::move(*result));

    auto rect = Rect::fromLTRB(10.0f, 10.0f, 190.0f, 190.0f);
    auto path = Path::fromRect(*rect);

    auto pixmap = Pixmap::fromSize(200, 200);
    ASSERT_TRUE(pixmap.has_value());
    auto mut = pixmap->mutableView();
    Painter::fillPath(mut, path, paint, FillRule::Winding, Transform::identity());

    EXPECT_GOLDEN_MATCH(*pixmap, "gradients/two-stops-linear-reflect-hq.png");
}

TEST(GradientsTest, ThreeStopsEvenlySpacedHq) {
    Paint paint;
    paint.forceHqPipeline = true;
    paint.antiAlias = false;

    auto result = LinearGradient::create(
        Point::fromXY(10.0f, 10.0f),
        Point::fromXY(190.0f, 190.0f),
        {
            GradientStop::create(0.25f, Color::fromRgba8(50, 127, 150, 200)),
            GradientStop::create(0.50f, Color::fromRgba8(220, 140, 75, 180)),
            GradientStop::create(0.75f, Color::fromRgba8(40, 180, 55, 160)),
        },
        SpreadMode::Pad,
        Transform::identity());
    ASSERT_TRUE(result.has_value());
    paint.shader = std::get<LinearGradient>(std::move(*result));

    auto rect = Rect::fromLTRB(10.0f, 10.0f, 190.0f, 190.0f);
    auto path = Path::fromRect(*rect);

    auto pixmap = Pixmap::fromSize(200, 200);
    ASSERT_TRUE(pixmap.has_value());
    auto mut = pixmap->mutableView();
    Painter::fillPath(mut, path, paint, FillRule::Winding, Transform::identity());

    EXPECT_GOLDEN_MATCH(*pixmap, "gradients/three-stops-evenly-spaced-hq.png");
}

TEST(GradientsTest, TwoStopsUnevenlySpacedHq) {
    Paint paint;
    paint.forceHqPipeline = true;
    paint.antiAlias = false;

    auto result = LinearGradient::create(
        Point::fromXY(10.0f, 10.0f),
        Point::fromXY(190.0f, 190.0f),
        {
            GradientStop::create(0.25f, Color::fromRgba8(50, 127, 150, 200)),
            GradientStop::create(0.75f, Color::fromRgba8(220, 140, 75, 180)),
        },
        SpreadMode::Pad,
        Transform::identity());
    ASSERT_TRUE(result.has_value());
    paint.shader = std::get<LinearGradient>(std::move(*result));

    auto rect = Rect::fromLTRB(10.0f, 10.0f, 190.0f, 190.0f);
    auto path = Path::fromRect(*rect);

    auto pixmap = Pixmap::fromSize(200, 200);
    ASSERT_TRUE(pixmap.has_value());
    auto mut = pixmap->mutableView();
    Painter::fillPath(mut, path, paint, FillRule::Winding, Transform::identity());

    EXPECT_GOLDEN_MATCH(*pixmap, "gradients/two-stops-unevenly-spaced-hq.png");
}

// ============================================================================
// Radial gradient tests
// The radial gradient is only supported by the high quality pipeline.
// Therefore we do not have a lq/hq split (except for simple_radial).
// ============================================================================

TEST(GradientsTest, WellBehavedRadial) {
    Paint paint;
    paint.antiAlias = false;

    auto result = RadialGradient::create(
        Point::fromXY(100.0f, 100.0f),
        0.0f,
        Point::fromXY(120.0f, 80.0f),
        100.0f,
        {
            GradientStop::create(0.25f, Color::fromRgba8(50, 127, 150, 200)),
            GradientStop::create(0.75f, Color::fromRgba8(220, 140, 75, 180)),
        },
        SpreadMode::Pad,
        Transform::identity());
    ASSERT_TRUE(result.has_value());
    paint.shader = std::get<RadialGradient>(std::move(*result));

    auto rect = Rect::fromLTRB(10.0f, 10.0f, 190.0f, 190.0f);
    auto path = Path::fromRect(*rect);

    auto pixmap = Pixmap::fromSize(200, 200);
    ASSERT_TRUE(pixmap.has_value());
    auto mut = pixmap->mutableView();
    Painter::fillPath(mut, path, paint, FillRule::Winding, Transform::identity());

    EXPECT_GOLDEN_MATCH(*pixmap, "gradients/well-behaved-radial.png");
}

TEST(GradientsTest, FocalOnCircleRadial) {
    Paint paint;
    paint.antiAlias = false;

    auto result = RadialGradient::create(
        Point::fromXY(100.0f, 100.0f),
        0.0f,
        Point::fromXY(120.0f, 80.0f),
        28.29f,  // This radius forces the required pipeline stage.
        {
            GradientStop::create(0.25f, Color::fromRgba8(50, 127, 150, 200)),
            GradientStop::create(0.75f, Color::fromRgba8(220, 140, 75, 180)),
        },
        SpreadMode::Pad,
        Transform::identity());
    ASSERT_TRUE(result.has_value());
    paint.shader = std::get<RadialGradient>(std::move(*result));

    auto rect = Rect::fromLTRB(10.0f, 10.0f, 190.0f, 190.0f);
    auto path = Path::fromRect(*rect);

    auto pixmap = Pixmap::fromSize(200, 200);
    ASSERT_TRUE(pixmap.has_value());
    auto mut = pixmap->mutableView();
    Painter::fillPath(mut, path, paint, FillRule::Winding, Transform::identity());

    EXPECT_GOLDEN_MATCH(*pixmap, "gradients/focal-on-circle-radial.png");
}

TEST(GradientsTest, ConicalGreaterRadial) {
    Paint paint;
    paint.antiAlias = false;

    auto result = RadialGradient::create(
        Point::fromXY(100.0f, 100.0f),
        0.0f,
        Point::fromXY(120.0f, 80.0f),
        10.0f,  // This radius forces the required pipeline stage.
        {
            GradientStop::create(0.25f, Color::fromRgba8(50, 127, 150, 200)),
            GradientStop::create(0.75f, Color::fromRgba8(220, 140, 75, 180)),
        },
        SpreadMode::Pad,
        Transform::identity());
    ASSERT_TRUE(result.has_value());
    paint.shader = std::get<RadialGradient>(std::move(*result));

    auto rect = Rect::fromLTRB(10.0f, 10.0f, 190.0f, 190.0f);
    auto path = Path::fromRect(*rect);

    auto pixmap = Pixmap::fromSize(200, 200);
    ASSERT_TRUE(pixmap.has_value());
    auto mut = pixmap->mutableView();
    Painter::fillPath(mut, path, paint, FillRule::Winding, Transform::identity());

    EXPECT_GOLDEN_MATCH(*pixmap, "gradients/conical-greater-radial.png");
}

TEST(GradientsTest, SimpleRadialLq) {
    Paint paint;
    paint.antiAlias = false;

    auto result = RadialGradient::create(
        Point::fromXY(100.0f, 100.0f),
        0.0f,
        Point::fromXY(100.0f, 100.0f),
        100.0f,
        {
            GradientStop::create(0.25f, Color::fromRgba8(50, 127, 150, 200)),
            GradientStop::create(1.00f, Color::fromRgba8(220, 140, 75, 180)),
        },
        SpreadMode::Pad,
        Transform::identity());
    ASSERT_TRUE(result.has_value());
    paint.shader = std::get<RadialGradient>(std::move(*result));

    auto rect = Rect::fromLTRB(10.0f, 10.0f, 190.0f, 190.0f);
    auto path = Path::fromRect(*rect);

    auto pixmap = Pixmap::fromSize(200, 200);
    ASSERT_TRUE(pixmap.has_value());
    auto mut = pixmap->mutableView();
    Painter::fillPath(mut, path, paint, FillRule::Winding, Transform::identity());

    EXPECT_GOLDEN_MATCH(*pixmap, "gradients/simple-radial-lq.png");
}

TEST(GradientsTest, SimpleRadialHq) {
    Paint paint;
    paint.forceHqPipeline = true;
    paint.antiAlias = false;

    auto result = RadialGradient::create(
        Point::fromXY(100.0f, 100.0f),
        0.0f,
        Point::fromXY(100.0f, 100.0f),
        100.0f,
        {
            GradientStop::create(0.25f, Color::fromRgba8(50, 127, 150, 200)),
            GradientStop::create(1.00f, Color::fromRgba8(220, 140, 75, 180)),
        },
        SpreadMode::Pad,
        Transform::identity());
    ASSERT_TRUE(result.has_value());
    paint.shader = std::get<RadialGradient>(std::move(*result));

    auto rect = Rect::fromLTRB(10.0f, 10.0f, 190.0f, 190.0f);
    auto path = Path::fromRect(*rect);

    auto pixmap = Pixmap::fromSize(200, 200);
    ASSERT_TRUE(pixmap.has_value());
    auto mut = pixmap->mutableView();
    Painter::fillPath(mut, path, paint, FillRule::Winding, Transform::identity());

    EXPECT_GOLDEN_MATCH(*pixmap, "gradients/simple-radial-hq.png");
}

TEST(GradientsTest, SimpleRadialWithTsHq) {
    Paint paint;
    paint.forceHqPipeline = true;
    paint.antiAlias = false;

    auto result = RadialGradient::create(
        Point::fromXY(100.0f, 100.0f),
        0.0f,
        Point::fromXY(100.0f, 100.0f),
        100.0f,
        {
            GradientStop::create(0.25f, Color::fromRgba8(50, 127, 150, 200)),
            GradientStop::create(1.00f, Color::fromRgba8(220, 140, 75, 180)),
        },
        SpreadMode::Pad,
        Transform::fromRow(2.0f, 0.3f, -0.7f, 1.2f, 10.5f, -12.3f));
    ASSERT_TRUE(result.has_value());
    paint.shader = std::get<RadialGradient>(std::move(*result));

    auto rect = Rect::fromLTRB(10.0f, 10.0f, 190.0f, 190.0f);
    auto path = Path::fromRect(*rect);

    auto pixmap = Pixmap::fromSize(200, 200);
    ASSERT_TRUE(pixmap.has_value());
    auto mut = pixmap->mutableView();
    Painter::fillPath(mut, path, paint, FillRule::Winding, Transform::identity());

    EXPECT_GOLDEN_MATCH(*pixmap, "gradients/simple-radial-with-ts-hq.png");
}

// Gradient doesn't add the Premultiply stage when all stops are opaque.
// But it checks colors only on creation, so we have to recheck them after
// calling applyShaderOpacity.
TEST(GradientsTest, globalOpacity) {
    Paint paint;
    paint.antiAlias = false;

    auto result = RadialGradient::create(
        Point::fromXY(100.0f, 100.0f),
        0.0f,
        Point::fromXY(100.0f, 100.0f),
        100.0f,
        {
            GradientStop::create(0.25f, Color::fromRgba8(50, 127, 150, 255)),
            GradientStop::create(1.00f, Color::fromRgba8(220, 140, 75, 255)),
        },
        SpreadMode::Pad,
        Transform::identity());
    ASSERT_TRUE(result.has_value());
    paint.shader = std::get<RadialGradient>(std::move(*result));
    applyShaderOpacity(paint.shader, 0.5f);

    auto rect = Rect::fromLTRB(10.0f, 10.0f, 190.0f, 190.0f);
    auto path = Path::fromRect(*rect);

    auto pixmap = Pixmap::fromSize(200, 200);
    ASSERT_TRUE(pixmap.has_value());
    auto mut = pixmap->mutableView();
    Painter::fillPath(mut, path, paint, FillRule::Winding, Transform::identity());

    EXPECT_GOLDEN_MATCH(*pixmap, "gradients/global-opacity.png");
}

TEST(GradientsTest, StripGradient) {
    // Equal radii, different centers creates a Strip gradient
    Paint paint;
    paint.antiAlias = false;

    auto result = RadialGradient::create(
        Point::fromXY(50.0f, 100.0f),
        50.0f,
        Point::fromXY(150.0f, 100.0f),
        50.0f,
        {
            GradientStop::create(0.0f, Color::fromRgba8(50, 127, 150, 200)),
            GradientStop::create(1.0f, Color::fromRgba8(220, 140, 75, 180)),
        },
        SpreadMode::Pad,
        Transform::identity());
    ASSERT_TRUE(result.has_value());
    paint.shader = std::get<RadialGradient>(std::move(*result));

    auto rect = Rect::fromLTRB(10.0f, 10.0f, 190.0f, 190.0f);
    auto path = Path::fromRect(*rect);

    auto pixmap = Pixmap::fromSize(200, 200);
    ASSERT_TRUE(pixmap.has_value());
    auto mut = pixmap->mutableView();
    Painter::fillPath(mut, path, paint, FillRule::Winding, Transform::identity());

    EXPECT_GOLDEN_MATCH(*pixmap, "gradients/strip-gradient.png");
}

TEST(GradientsTest, ConcentricRadial) {
    // Same center, non-zero start radius (concentric gradient)
    Paint paint;
    paint.antiAlias = false;

    auto result = RadialGradient::create(
        Point::fromXY(100.0f, 100.0f),
        30.0f,
        Point::fromXY(100.0f, 100.0f),
        90.0f,
        {
            GradientStop::create(0.0f, Color::fromRgba8(50, 127, 150, 200)),
            GradientStop::create(1.0f, Color::fromRgba8(220, 140, 75, 180)),
        },
        SpreadMode::Pad,
        Transform::identity());
    ASSERT_TRUE(result.has_value());
    paint.shader = std::get<RadialGradient>(std::move(*result));

    auto rect = Rect::fromLTRB(10.0f, 10.0f, 190.0f, 190.0f);
    auto path = Path::fromRect(*rect);

    auto pixmap = Pixmap::fromSize(200, 200);
    ASSERT_TRUE(pixmap.has_value());
    auto mut = pixmap->mutableView();
    Painter::fillPath(mut, path, paint, FillRule::Winding, Transform::identity());

    EXPECT_GOLDEN_MATCH(*pixmap, "gradients/concentric-radial.png");
}

TEST(GradientsTest, ConicalSmallerRadial) {
    // Configuration that triggers XYTo2PtConicalSmaller stage
    // r0=60, r1=30, distance=50
    Paint paint;
    paint.antiAlias = false;

    auto result = RadialGradient::create(
        Point::fromXY(100.0f, 100.0f),
        60.0f,
        Point::fromXY(150.0f, 100.0f),
        30.0f,
        {
            GradientStop::create(0.0f, Color::fromRgba8(50, 127, 150, 200)),
            GradientStop::create(1.0f, Color::fromRgba8(220, 140, 75, 180)),
        },
        SpreadMode::Pad,
        Transform::identity());
    ASSERT_TRUE(result.has_value());
    paint.shader = std::get<RadialGradient>(std::move(*result));

    auto rect = Rect::fromLTRB(10.0f, 10.0f, 190.0f, 190.0f);
    auto path = Path::fromRect(*rect);

    auto pixmap = Pixmap::fromSize(200, 200);
    ASSERT_TRUE(pixmap.has_value());
    auto mut = pixmap->mutableView();
    Painter::fillPath(mut, path, paint, FillRule::Winding, Transform::identity());

    EXPECT_GOLDEN_MATCH(*pixmap, "gradients/conical-smaller-radial.png");
}

// ============================================================================
// Sweep gradient tests
// ============================================================================

TEST(GradientsTest, SweepGradient) {
    Paint paint;
    paint.antiAlias = false;

    auto result = SweepGradient::create(
        Point::fromXY(100.0f, 100.0f),
        135.0f,
        225.0f,
        {
            GradientStop::create(0.0f, Color::fromRgba8(50, 127, 150, 200)),
            GradientStop::create(1.0f, Color::fromRgba8(220, 140, 75, 180)),
        },
        SpreadMode::Pad,
        Transform::identity());
    ASSERT_TRUE(result.has_value());
    paint.shader = std::get<SweepGradient>(std::move(*result));

    auto rect = Rect::fromLTRB(10.0f, 10.0f, 190.0f, 190.0f);
    auto path = Path::fromRect(*rect);

    auto pixmap = Pixmap::fromSize(200, 200);
    ASSERT_TRUE(pixmap.has_value());
    auto mut = pixmap->mutableView();
    Painter::fillPath(mut, path, paint, FillRule::Winding, Transform::identity());

    EXPECT_GOLDEN_MATCH(*pixmap, "gradients/sweep-gradient.png");
}

TEST(GradientsTest, SweepGradientFull) {
    Paint paint;
    paint.antiAlias = false;

    auto result = SweepGradient::create(
        Point::fromXY(100.0f, 100.0f),
        0.0f,
        360.0f,
        {
            GradientStop::create(0.0f, Color::fromRgba8(50, 127, 150, 200)),
            GradientStop::create(1.0f, Color::fromRgba8(220, 140, 75, 180)),
        },
        SpreadMode::Pad,
        Transform::identity());
    ASSERT_TRUE(result.has_value());
    paint.shader = std::get<SweepGradient>(std::move(*result));

    auto rect = Rect::fromLTRB(10.0f, 10.0f, 190.0f, 190.0f);
    auto path = Path::fromRect(*rect);

    auto pixmap = Pixmap::fromSize(200, 200);
    ASSERT_TRUE(pixmap.has_value());
    auto mut = pixmap->mutableView();
    Painter::fillPath(mut, path, paint, FillRule::Winding, Transform::identity());

    EXPECT_GOLDEN_MATCH(*pixmap, "gradients/sweep-gradient-full.png");
}
