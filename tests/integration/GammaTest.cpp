#include <gtest/gtest.h>
#include "GoldenTestHelper.h"
#include "tiny_skia/Color.h"
#include "tiny_skia/Geom.h"
#include "tiny_skia/Painter.h"
#include "tiny_skia/Path.h"
#include "tiny_skia/PathBuilder.h"
#include "tiny_skia/Pixmap.h"
#include "tiny_skia/Stroke.h"
#include "tiny_skia/shaders/Shaders.h"

#include <variant>
#include <vector>

using namespace tiny_skia;

// ---------------------------------------------------------------------------
// gamma
// Renders multiple strokes with different color spaces and gradient shaders
// onto a 500x60 pixmap.
// ---------------------------------------------------------------------------
TEST(GammaTest, gamma) {
    Paint paint;
    Stroke stroke;  // default width = 1.0
    Stroke wide;
    wide.width = 3.0f;

    // Solid color shader.
    Color solid = Color::fromRgba8(255, 100, 20, 255);

    // 2-stop linear gradient.
    auto grad2Result = LinearGradient::create(
        Point{50.0f, 2.0f},
        Point{450.0f, 2.0f},
        {
            GradientStop::create(0.0f, Color::fromRgba8(255, 0, 0, 255)),
            GradientStop::create(1.0f, Color::fromRgba8(0, 255, 0, 255)),
        },
        SpreadMode::Pad,
        Transform::identity());
    ASSERT_TRUE(grad2Result.has_value());
    auto grad2 = std::get<LinearGradient>(std::move(*grad2Result));

    // 3-stop linear gradient.
    auto grad3Result = LinearGradient::create(
        Point{50.0f, 2.0f},
        Point{450.0f, 2.0f},
        {
            GradientStop::create(0.0f, Color::fromRgba8(255, 0, 0, 255)),
            GradientStop::create(0.5f, Color::fromRgba8(0, 0, 255, 255)),
            GradientStop::create(1.0f, Color::fromRgba8(128, 128, 128, 128)),
        },
        SpreadMode::Pad,
        Transform::identity());
    ASSERT_TRUE(grad3Result.has_value());
    auto grad3 = std::get<LinearGradient>(std::move(*grad3Result));

    auto pixmap = Pixmap::fromSize(500, 60);
    ASSERT_TRUE(pixmap.has_value());
    pixmap->fill(Color::black);

    PathBuilder pb;
    pb.moveTo(20.0f, 2.0f);
    pb.lineTo(480.0f, 3.0f);
    auto path = pb.finish();
    ASSERT_TRUE(path.has_value());

    ColorSpace colors[] = {
        ColorSpace::Linear,
        ColorSpace::Gamma2,
        ColorSpace::SimpleSRGB,
        ColorSpace::FullSRGBGamma,
    };

    for (int i = 0; i < 4; ++i) {
        auto xf = Transform::fromTranslate(0.0f, 4.0f * static_cast<float>(i));

        paint.colorspace = colors[i];
        paint.shader = solid;
        auto mut = pixmap->mutableView();
        Painter::strokePath(mut, *path, paint, stroke, xf);

        auto xf2 = Transform::fromTranslate(0.0f, 20.0f + 10.0f * static_cast<float>(i));
        paint.shader = grad2;
        Painter::strokePath(mut, *path, paint, wide, xf2);

        auto xf3 = Transform::fromTranslate(0.0f, 22.5f + 10.0f * static_cast<float>(i));
        paint.shader = grad3;
        Painter::strokePath(mut, *path, paint, wide, xf3);
    }

    EXPECT_GOLDEN_MATCH(*pixmap, "gamma.png");
}
