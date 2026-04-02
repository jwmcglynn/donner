#include <gtest/gtest.h>
#include "tiny_skia/Color.h"
#include "tiny_skia/Geom.h"
#include "tiny_skia/Painter.h"
#include "tiny_skia/Path.h"
#include "tiny_skia/PathBuilder.h"
#include "tiny_skia/Pixmap.h"
#include "tiny_skia/Stroke.h"
#include "tiny_skia/shaders/Shaders.h"

using namespace tiny_skia;

namespace {

// Helper to compare verbs vectors.
void expectVerbs(const Path& path, const std::vector<PathVerb>& expected) {
    auto verbs = path.verbs();
    ASSERT_EQ(verbs.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(verbs[i], expected[i]) << "Verb mismatch at index " << i;
    }
}

// Helper to compare points vectors.
void expectPoints(const Path& path, const std::vector<Point>& expected) {
    auto points = path.points();
    ASSERT_EQ(points.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_FLOAT_EQ(points[i].x, expected[i].x) << "Point x mismatch at index " << i;
        EXPECT_FLOAT_EQ(points[i].y, expected[i].y) << "Point y mismatch at index " << i;
    }
}

// Helper to compare bounds.
void expectBounds(const Path& path, float left, float top, float right, float bottom) {
    auto bounds = path.bounds();
    EXPECT_FLOAT_EQ(bounds.left(), left);
    EXPECT_FLOAT_EQ(bounds.top(), top);
    EXPECT_FLOAT_EQ(bounds.right(), right);
    EXPECT_FLOAT_EQ(bounds.bottom(), bottom);
}

}  // namespace

// ---------------------------------------------------------------------------
// empty
// ---------------------------------------------------------------------------
TEST(PathTest, empty) {
    PathBuilder pb;
    EXPECT_FALSE(pb.finish().has_value());
}

// ---------------------------------------------------------------------------
// line
// ---------------------------------------------------------------------------
TEST(PathTest, line) {
    PathBuilder pb;
    pb.moveTo(10.0f, 20.0f);
    pb.lineTo(30.0f, 40.0f);
    auto path = pb.finish();
    ASSERT_TRUE(path.has_value());

    expectBounds(*path, 10.0f, 20.0f, 30.0f, 40.0f);
    expectVerbs(*path, {PathVerb::Move, PathVerb::Line});
    expectPoints(*path, {
        Point::fromXY(10.0f, 20.0f),
        Point::fromXY(30.0f, 40.0f),
    });
}

// ---------------------------------------------------------------------------
// no_move_before_line
// ---------------------------------------------------------------------------
TEST(PathTest, NoMoveBeforeLine) {
    PathBuilder pb;
    pb.lineTo(30.0f, 40.0f);
    auto path = pb.finish();
    ASSERT_TRUE(path.has_value());

    expectBounds(*path, 0.0f, 0.0f, 30.0f, 40.0f);
    expectVerbs(*path, {PathVerb::Move, PathVerb::Line});
    expectPoints(*path, {
        Point::fromXY(0.0f, 0.0f),
        Point::fromXY(30.0f, 40.0f),
    });
}

// ---------------------------------------------------------------------------
// no_move_before_quad
// ---------------------------------------------------------------------------
TEST(PathTest, NoMoveBeforeQuad) {
    PathBuilder pb;
    pb.quadTo(40.0f, 30.0f, 60.0f, 75.0f);
    auto path = pb.finish();
    ASSERT_TRUE(path.has_value());

    expectBounds(*path, 0.0f, 0.0f, 60.0f, 75.0f);
    expectVerbs(*path, {PathVerb::Move, PathVerb::Quad});
    expectPoints(*path, {
        Point::fromXY(0.0f, 0.0f),
        Point::fromXY(40.0f, 30.0f),
        Point::fromXY(60.0f, 75.0f),
    });
}

// ---------------------------------------------------------------------------
// no_move_before_cubic
// ---------------------------------------------------------------------------
TEST(PathTest, NoMoveBeforeCubic) {
    PathBuilder pb;
    pb.cubicTo(40.0f, 30.0f, 60.0f, 75.0f, 33.0f, 66.0f);
    auto path = pb.finish();
    ASSERT_TRUE(path.has_value());

    expectBounds(*path, 0.0f, 0.0f, 60.0f, 75.0f);
    expectVerbs(*path, {PathVerb::Move, PathVerb::Cubic});
    expectPoints(*path, {
        Point::fromXY(0.0f, 0.0f),
        Point::fromXY(40.0f, 30.0f),
        Point::fromXY(60.0f, 75.0f),
        Point::fromXY(33.0f, 66.0f),
    });
}

// ---------------------------------------------------------------------------
// no_move_before_close
// ---------------------------------------------------------------------------
TEST(PathTest, NoMoveBeforeClose) {
    PathBuilder pb;
    pb.close();
    EXPECT_FALSE(pb.finish().has_value());
}

// ---------------------------------------------------------------------------
// double_close
// ---------------------------------------------------------------------------
TEST(PathTest, DoubleClose) {
    PathBuilder pb;
    pb.moveTo(10.0f, 10.0f);
    pb.lineTo(20.0f, 10.0f);
    pb.lineTo(20.0f, 20.0f);
    pb.close();
    pb.close();
    auto path = pb.finish();
    ASSERT_TRUE(path.has_value());

    expectBounds(*path, 10.0f, 10.0f, 20.0f, 20.0f);
    expectVerbs(*path, {PathVerb::Move, PathVerb::Line, PathVerb::Line, PathVerb::Close});
    expectPoints(*path, {
        Point::fromXY(10.0f, 10.0f),
        Point::fromXY(20.0f, 10.0f),
        Point::fromXY(20.0f, 20.0f),
    });
}

// ---------------------------------------------------------------------------
// double_move_to_1
// ---------------------------------------------------------------------------
TEST(PathTest, DoubleMoveTo1) {
    PathBuilder pb;
    pb.moveTo(10.0f, 20.0f);
    pb.moveTo(30.0f, 40.0f);
    EXPECT_FALSE(pb.finish().has_value());
}

// ---------------------------------------------------------------------------
// double_move_to_2
// ---------------------------------------------------------------------------
TEST(PathTest, DoubleMoveTo2) {
    PathBuilder pb;
    pb.moveTo(10.0f, 20.0f);
    pb.moveTo(20.0f, 10.0f);
    pb.lineTo(30.0f, 40.0f);
    auto path = pb.finish();
    ASSERT_TRUE(path.has_value());

    expectBounds(*path, 20.0f, 10.0f, 30.0f, 40.0f);
    expectVerbs(*path, {PathVerb::Move, PathVerb::Line});
    expectPoints(*path, {
        Point::fromXY(20.0f, 10.0f),
        Point::fromXY(30.0f, 40.0f),
    });
}

// ---------------------------------------------------------------------------
// two_contours
// ---------------------------------------------------------------------------
TEST(PathTest, TwoContours) {
    PathBuilder pb;
    pb.moveTo(10.0f, 20.0f);
    pb.lineTo(30.0f, 40.0f);
    pb.moveTo(100.0f, 200.0f);
    pb.lineTo(300.0f, 400.0f);
    auto path = pb.finish();
    ASSERT_TRUE(path.has_value());

    expectBounds(*path, 10.0f, 20.0f, 300.0f, 400.0f);
    expectVerbs(*path, {PathVerb::Move, PathVerb::Line, PathVerb::Move, PathVerb::Line});
    expectPoints(*path, {
        Point::fromXY(10.0f, 20.0f),
        Point::fromXY(30.0f, 40.0f),
        Point::fromXY(100.0f, 200.0f),
        Point::fromXY(300.0f, 400.0f),
    });
}

// ---------------------------------------------------------------------------
// two_closed_contours
// ---------------------------------------------------------------------------
TEST(PathTest, TwoClosedContours) {
    PathBuilder pb;
    pb.moveTo(10.0f, 20.0f);
    pb.lineTo(30.0f, 40.0f);
    pb.close();
    pb.moveTo(100.0f, 200.0f);
    pb.lineTo(300.0f, 400.0f);
    pb.close();
    auto path = pb.finish();
    ASSERT_TRUE(path.has_value());

    expectBounds(*path, 10.0f, 20.0f, 300.0f, 400.0f);
    expectVerbs(*path, {
        PathVerb::Move, PathVerb::Line, PathVerb::Close,
        PathVerb::Move, PathVerb::Line, PathVerb::Close,
    });
    expectPoints(*path, {
        Point::fromXY(10.0f, 20.0f),
        Point::fromXY(30.0f, 40.0f),
        Point::fromXY(100.0f, 200.0f),
        Point::fromXY(300.0f, 400.0f),
    });
}

// ---------------------------------------------------------------------------
// line_after_close
// ---------------------------------------------------------------------------
TEST(PathTest, LineAfterClose) {
    PathBuilder pb;
    pb.moveTo(10.0f, 20.0f);
    pb.lineTo(30.0f, 40.0f);
    pb.close();
    pb.lineTo(20.0f, 20.0f);
    auto path = pb.finish();
    ASSERT_TRUE(path.has_value());

    expectBounds(*path, 10.0f, 20.0f, 30.0f, 40.0f);
    expectVerbs(*path, {
        PathVerb::Move, PathVerb::Line, PathVerb::Close,
        PathVerb::Move, PathVerb::Line,
    });
    expectPoints(*path, {
        Point::fromXY(10.0f, 20.0f),
        Point::fromXY(30.0f, 40.0f),
        Point::fromXY(10.0f, 20.0f),
        Point::fromXY(20.0f, 20.0f),
    });
}

// ---------------------------------------------------------------------------
// hor_line
// ---------------------------------------------------------------------------
TEST(PathTest, HorLine) {
    PathBuilder pb;
    pb.moveTo(10.0f, 10.0f);
    pb.lineTo(20.0f, 10.0f);
    auto path = pb.finish();
    ASSERT_TRUE(path.has_value());

    expectBounds(*path, 10.0f, 10.0f, 20.0f, 10.0f);
    expectVerbs(*path, {PathVerb::Move, PathVerb::Line});
    expectPoints(*path, {
        Point::fromXY(10.0f, 10.0f),
        Point::fromXY(20.0f, 10.0f),
    });
}

// ---------------------------------------------------------------------------
// ver_line
// ---------------------------------------------------------------------------
TEST(PathTest, VerLine) {
    PathBuilder pb;
    pb.moveTo(10.0f, 10.0f);
    pb.lineTo(10.0f, 20.0f);
    auto path = pb.finish();
    ASSERT_TRUE(path.has_value());

    expectBounds(*path, 10.0f, 10.0f, 10.0f, 20.0f);
    expectVerbs(*path, {PathVerb::Move, PathVerb::Line});
    expectPoints(*path, {
        Point::fromXY(10.0f, 10.0f),
        Point::fromXY(10.0f, 20.0f),
    });
}

// ---------------------------------------------------------------------------
// translate
// ---------------------------------------------------------------------------
TEST(PathTest, translate) {
    PathBuilder pb;
    pb.moveTo(10.0f, 20.0f);
    pb.lineTo(30.0f, 40.0f);
    auto path = pb.finish();
    ASSERT_TRUE(path.has_value());

    auto transformed = path->transform(Transform::fromTranslate(10.0f, 20.0f));
    ASSERT_TRUE(transformed.has_value());

    expectVerbs(*transformed, {PathVerb::Move, PathVerb::Line});
    expectPoints(*transformed, {
        Point::fromXY(20.0f, 40.0f),
        Point::fromXY(40.0f, 60.0f),
    });
}

// ---------------------------------------------------------------------------
// scale
// ---------------------------------------------------------------------------
TEST(PathTest, scale) {
    PathBuilder pb;
    pb.moveTo(10.0f, 20.0f);
    pb.lineTo(30.0f, 40.0f);
    auto path = pb.finish();
    ASSERT_TRUE(path.has_value());

    auto transformed = path->transform(Transform::fromScale(2.0f, 0.5f));
    ASSERT_TRUE(transformed.has_value());

    expectVerbs(*transformed, {PathVerb::Move, PathVerb::Line});
    expectPoints(*transformed, {
        Point::fromXY(20.0f, 10.0f),
        Point::fromXY(60.0f, 20.0f),
    });
}

// ---------------------------------------------------------------------------
// transform
// ---------------------------------------------------------------------------
TEST(PathTest, transform) {
    PathBuilder pb;
    pb.moveTo(10.0f, 20.0f);
    pb.lineTo(30.0f, 40.0f);
    auto path = pb.finish();
    ASSERT_TRUE(path.has_value());

    auto transformed = path->transform(
        Transform::fromRow(2.0f, 0.7f, -0.3f, 0.5f, 10.0f, 20.0f));
    ASSERT_TRUE(transformed.has_value());

    expectVerbs(*transformed, {PathVerb::Move, PathVerb::Line});
    expectPoints(*transformed, {
        Point::fromXY(24.0f, 37.0f),
        Point::fromXY(58.0f, 61.0f),
    });
}

// ---------------------------------------------------------------------------
// invalid_transform
// ---------------------------------------------------------------------------
TEST(PathTest, InvalidTransform) {
    PathBuilder pb;
    pb.moveTo(10.0f, 20.0f);
    pb.lineTo(30.0f, 40.0f);
    auto path = pb.finish();
    ASSERT_TRUE(path.has_value());

    // Will produce infinity.
    auto transformed = path->transform(
        Transform::fromScale(std::numeric_limits<float>::max(),
                             std::numeric_limits<float>::max()));
    EXPECT_FALSE(transformed.has_value());
}

// ---------------------------------------------------------------------------
// circle
// ---------------------------------------------------------------------------
TEST(PathTest, circle) {
    auto path = Path::fromCircle(250.0f, 250.0f, 300.0f);
    EXPECT_TRUE(path.has_value());  // Must not panic.
}

// ---------------------------------------------------------------------------
// large_circle
// ---------------------------------------------------------------------------
TEST(PathTest, LargeCircle) {
    auto path = Path::fromCircle(250.0f, 250.0f, 2000.0f);
    EXPECT_TRUE(path.has_value());  // Must not panic.
}

// ---------------------------------------------------------------------------
// tight_bounds_1
// ---------------------------------------------------------------------------
TEST(PathTest, TightBounds1) {
    PathBuilder pb;
    pb.moveTo(50.0f, 85.0f);
    pb.lineTo(65.0f, 135.0f);
    pb.lineTo(150.0f, 135.0f);
    pb.lineTo(85.0f, 135.0f);
    pb.quadTo(100.0f, 45.0f, 50.0f, 85.0f);
    auto path = pb.finish();
    ASSERT_TRUE(path.has_value());

    // Check regular bounds (uses control points).
    expectBounds(*path, 50.0f, 45.0f, 150.0f, 135.0f);

    // Check tight bounds (finds curve extrema).
    auto tightBounds = path->computeTightBounds();
    ASSERT_TRUE(tightBounds.has_value());
    EXPECT_FLOAT_EQ(tightBounds->left(), 50.0f);
    EXPECT_FLOAT_EQ(tightBounds->top(), 72.692307f);
    EXPECT_FLOAT_EQ(tightBounds->right(), 150.0f);
    EXPECT_FLOAT_EQ(tightBounds->bottom(), 135.0f);
}

// ---------------------------------------------------------------------------
// tight_bounds_2
// ---------------------------------------------------------------------------
TEST(PathTest, TightBounds2) {
    PathBuilder pb;
    pb.moveTo(-19.309214f, 72.11173f);
    pb.cubicTo(-24.832062f, 67.477516f, -20.490944f, 62.16584f, -9.61306f, 60.247776f);
    pb.cubicTo(1.2648277f, 58.329712f, 14.560249f, 60.53159f, 20.083096f, 65.16581f);
    pb.cubicTo(14.560249f, 60.53159f, 18.901363f, 55.219913f, 29.779247f, 53.30185f);
    pb.cubicTo(40.65713f, 51.383785f, 53.952557f, 53.585663f, 59.475407f, 58.21988f);
    pb.quadTo(74.4754f, 70.80637f, 50.083096f, 90.3388f);
    pb.quadTo(-4.3092155f, 84.69823f, -19.309214f, 72.11173f);
    pb.close();
    auto path = pb.finish();
    ASSERT_TRUE(path.has_value());

    auto tightBounds = path->computeTightBounds();
    ASSERT_TRUE(tightBounds.has_value());
    EXPECT_FLOAT_EQ(tightBounds->left(), -21.707121f);
    EXPECT_FLOAT_EQ(tightBounds->top(), 52.609154f);
    EXPECT_FLOAT_EQ(tightBounds->right(), -21.707121f + 86.894302f);
    EXPECT_FLOAT_EQ(tightBounds->bottom(), 52.609154f + 37.729645f);
}
