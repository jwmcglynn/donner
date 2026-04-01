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

#include <vector>

using namespace tiny_skia;

// ---------------------------------------------------------------------------
// crbug_140642
// We used to see this construct, and due to rounding as we accumulated
// our length, the loop where we apply the phase would run off the end of
// the array, since it relied on just -= each interval value, which did not
// behave as "expected". Now the code explicitly checks for walking off the
// end of that array.
// ---------------------------------------------------------------------------
TEST(SkiaDashTest, Crbug140642) {
    auto dash = StrokeDash::create(
        std::vector<float>{27734.0f, 35660.0f, 2157846850.0f, 247.0f},
        -248.135982067f);
    EXPECT_TRUE(dash.has_value());
}

// ---------------------------------------------------------------------------
// crbug_124652
// http://code.google.com/p/chromium/issues/detail?id=124652
// This particular test/bug only applies to the float case, where
// large values can "swamp" small ones.
// ---------------------------------------------------------------------------
TEST(SkiaDashTest, Crbug124652) {
    auto dash = StrokeDash::create(
        std::vector<float>{837099584.0f, 33450.0f},
        -10.0f);
    EXPECT_TRUE(dash.has_value());
}

// ---------------------------------------------------------------------------
// infinite_dash
// Extremely large path_length/dash_length ratios may cause infinite looping
// due to single precision rounding.
// ---------------------------------------------------------------------------
TEST(SkiaDashTest, InfiniteDash) {
    PathBuilder pb;
    pb.moveTo(0.0f, 5.0f);
    pb.lineTo(5000000.0f, 5.0f);
    auto path = pb.finish();
    ASSERT_TRUE(path.has_value());

    Paint paint;
    paint.setColorRgba8(50, 127, 150, 200);
    paint.antiAlias = true;

    Stroke stroke;
    stroke.dash = StrokeDash::create(std::vector<float>{0.2f, 0.2f}, 0.0f);

    auto pixmap = Pixmap::fromSize(100, 100);
    ASSERT_TRUE(pixmap.has_value());

    // Should not hang or crash. Doesn't draw anything visible.
    auto mut = pixmap->mutableView();
    Painter::strokePath(mut, *path, paint, stroke, Transform::identity());

    EXPECT_TRUE(true);
}
