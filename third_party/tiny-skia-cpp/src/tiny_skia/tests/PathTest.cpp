#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "tiny_skia/Path.h"
#include "tiny_skia/PathBuilder.h"
#include "tiny_skia/Stroke.h"
#include "tiny_skia/Stroker.h"

using ::testing::Optional;

// ---- computeTightBounds tests ----

TEST(PathTest, ComputeTightBoundsOnEmptyPathReturnsNullopt) {
  tiny_skia::Path path;
  EXPECT_EQ(path.computeTightBounds(), std::nullopt);
}

TEST(PathTest, ComputeTightBoundsOnLinePathMatchesBounds) {
  tiny_skia::PathBuilder builder;
  builder.moveTo(10.0f, 20.0f);
  builder.lineTo(30.0f, 40.0f);
  auto path = builder.finish();
  ASSERT_TRUE(path.has_value());

  auto tight = path->computeTightBounds();
  ASSERT_THAT(tight, Optional(testing::_));
  EXPECT_FLOAT_EQ(tight->left(), 10.0f);
  EXPECT_FLOAT_EQ(tight->top(), 20.0f);
  EXPECT_FLOAT_EQ(tight->right(), 30.0f);
  EXPECT_FLOAT_EQ(tight->bottom(), 40.0f);

  // For line-only paths, tight bounds == control point bounds.
  auto normal = path->bounds();
  EXPECT_FLOAT_EQ(tight->left(), normal.left());
  EXPECT_FLOAT_EQ(tight->top(), normal.top());
  EXPECT_FLOAT_EQ(tight->right(), normal.right());
  EXPECT_FLOAT_EQ(tight->bottom(), normal.bottom());
}

TEST(PathTest, ComputeTightBoundsOnQuadPathTighterThanBounds) {
  // A quadratic curve whose control point extends above the curve.
  // Move(0,0), Quad(50,100, 100,0): control point at (50,100) but
  // the curve never reaches y=100.
  tiny_skia::PathBuilder builder;
  builder.moveTo(0.0f, 0.0f);
  builder.quadTo(50.0f, 100.0f, 100.0f, 0.0f);
  auto path = builder.finish();
  ASSERT_TRUE(path.has_value());

  auto tight = path->computeTightBounds();
  ASSERT_THAT(tight, Optional(testing::_));
  // The curve's maximum y is 50 (at t=0.5), not 100.
  EXPECT_LT(tight->bottom(), 100.0f);
  EXPECT_NEAR(tight->bottom(), 50.0f, 0.01f);

  // Regular bounds include the control point.
  auto normal = path->bounds();
  EXPECT_FLOAT_EQ(normal.bottom(), 100.0f);
}

TEST(PathTest, ComputeTightBoundsOnCubicPathTighterThanBounds) {
  // Cubic with control points that overshoot.
  // Move(0,0), Cubic(0,100, 100,100, 100,0) — a symmetric arch.
  tiny_skia::PathBuilder builder;
  builder.moveTo(0.0f, 0.0f);
  builder.cubicTo(0.0f, 100.0f, 100.0f, 100.0f, 100.0f, 0.0f);
  auto path = builder.finish();
  ASSERT_TRUE(path.has_value());

  auto tight = path->computeTightBounds();
  ASSERT_THAT(tight, Optional(testing::_));
  // Max y of the curve is 75 (at t=0.5 for this symmetric cubic).
  EXPECT_LT(tight->bottom(), 100.0f);
  EXPECT_NEAR(tight->bottom(), 75.0f, 0.01f);

  // Regular bounds include control points at y=100.
  auto normal = path->bounds();
  EXPECT_FLOAT_EQ(normal.bottom(), 100.0f);
}

// ---- clear tests ----

TEST(PathTest, ClearReturnsEmptyPathBuilder) {
  tiny_skia::PathBuilder builder;
  builder.moveTo(10.0f, 20.0f);
  builder.lineTo(30.0f, 40.0f);
  auto path = builder.finish();
  ASSERT_TRUE(path.has_value());
  ASSERT_FALSE(path->empty());

  auto newBuilder = path->clear();
  EXPECT_TRUE(path->empty());       // Path should be empty after clear.
  EXPECT_TRUE(newBuilder.empty());  // Builder starts empty.

  // The returned builder should be usable.
  newBuilder.moveTo(1.0f, 2.0f);
  newBuilder.lineTo(3.0f, 4.0f);
  auto newPath = newBuilder.finish();
  ASSERT_TRUE(newPath.has_value());
  EXPECT_EQ(newPath->size(), 2u);
}

// ---- StrokeDash validation tests (ported from Rust dash.rs: test) ----

TEST(PathTest, StrokeDashCreateRejectsInvalidDashArrays) {
  // Empty array.
  EXPECT_FALSE(tiny_skia::StrokeDash::create({}, 0.0f).has_value());
  // Odd number of entries.
  EXPECT_FALSE(tiny_skia::StrokeDash::create({1.0f}, 0.0f).has_value());
  // Odd number of entries (3).
  EXPECT_FALSE(tiny_skia::StrokeDash::create({1.0f, 2.0f, 3.0f}, 0.0f).has_value());
  // Negative value in array.
  EXPECT_FALSE(tiny_skia::StrokeDash::create({1.0f, -2.0f}, 0.0f).has_value());
  // All zeros.
  EXPECT_FALSE(tiny_skia::StrokeDash::create({0.0f, 0.0f}, 0.0f).has_value());
  // Sum is zero (1 + -1 = 0).
  EXPECT_FALSE(tiny_skia::StrokeDash::create({1.0f, -1.0f}, 0.0f).has_value());
  // Infinite offset.
  EXPECT_FALSE(tiny_skia::StrokeDash::create({1.0f, 1.0f}, std::numeric_limits<float>::infinity())
                   .has_value());
  // Infinite value in array.
  EXPECT_FALSE(tiny_skia::StrokeDash::create({1.0f, std::numeric_limits<float>::infinity()}, 0.0f)
                   .has_value());
}

// Ported from Rust dash.rs: bug_26
TEST(PathTest, StrokeDashBug26DashingRegressionProducesValidPath) {
  tiny_skia::PathBuilder pb;
  pb.moveTo(665.54f, 287.3f);
  pb.lineTo(675.67f, 273.04f);
  pb.lineTo(675.52f, 271.32f);
  pb.lineTo(674.79f, 269.61f);
  pb.lineTo(674.05f, 268.04f);
  pb.lineTo(672.88f, 266.47f);
  pb.lineTo(671.27f, 264.9f);
  auto path = pb.finish();
  ASSERT_TRUE(path.has_value());

  auto strokeDash = tiny_skia::StrokeDash::create({6.0f, 4.5f}, 0.0f);
  ASSERT_TRUE(strokeDash.has_value());

  auto dashed = path->dash(*strokeDash, 1.0f);
  EXPECT_TRUE(dashed.has_value()) << "dashing a polyline with a valid dash should succeed";
}

// ---- Stroker tests (ported from Rust stroker.rs) ----

namespace {

// Helper to assert a PathSegment is a MoveTo with the given coordinates.
void expectMoveTo(const std::optional<tiny_skia::PathSegment>& seg, float x, float y,
                  const char* context) {
  ASSERT_TRUE(seg.has_value()) << context << ": expected MoveTo but got end of segments";
  EXPECT_EQ(seg->kind, tiny_skia::PathSegment::Kind::MoveTo) << context;
  EXPECT_NEAR(seg->pts[0].x, x, 1e-4f) << context << " x";
  EXPECT_NEAR(seg->pts[0].y, y, 1e-4f) << context << " y";
}

// Helper to assert a PathSegment is a LineTo with the given coordinates.
void expectLineTo(const std::optional<tiny_skia::PathSegment>& seg, float x, float y,
                  const char* context) {
  ASSERT_TRUE(seg.has_value()) << context << ": expected LineTo but got end of segments";
  EXPECT_EQ(seg->kind, tiny_skia::PathSegment::Kind::LineTo) << context;
  EXPECT_NEAR(seg->pts[0].x, x, 1e-4f) << context << " x";
  EXPECT_NEAR(seg->pts[0].y, y, 1e-4f) << context << " y";
}

// Helper to assert a PathSegment is a Close.
void expectClose(const std::optional<tiny_skia::PathSegment>& seg, const char* context) {
  ASSERT_TRUE(seg.has_value()) << context << ": expected Close but got end of segments";
  EXPECT_EQ(seg->kind, tiny_skia::PathSegment::Kind::Close) << context;
}

}  // namespace

// Ported from Rust stroker.rs: auto_close
// Exact segment-level verification of stroking a closed triangle with auto_close.
// This mirrors the Rust test which checks every segment's coordinates in order.
TEST(PathTest, StrokerAutoCloseTriangleProducesExpectedSegments) {
  // A triangle.
  tiny_skia::PathBuilder pb;
  pb.moveTo(10.0f, 10.0f);
  pb.lineTo(20.0f, 50.0f);
  pb.lineTo(30.0f, 10.0f);
  pb.close();
  auto path = pb.finish();
  ASSERT_TRUE(path.has_value());

  tiny_skia::Stroke stroke;
  auto strokePath = tiny_skia::PathStroker().stroke(*path, stroke, 1.0f);
  ASSERT_TRUE(strokePath.has_value()) << "stroking a triangle must succeed";

  tiny_skia::PathSegmentsIter iter(*strokePath);
  iter.setAutoClose(true);

  // Outer sub-path (Rust exact values from stroker.rs auto_close test).
  expectMoveTo(iter.next(), 10.485071f, 9.878732f, "outer[0] MoveTo");
  expectLineTo(iter.next(), 20.485071f, 49.878731f, "outer[1] LineTo");
  expectLineTo(iter.next(), 20.0f, 50.0f, "outer[2] LineTo");
  expectLineTo(iter.next(), 19.514929f, 49.878731f, "outer[3] LineTo");
  expectLineTo(iter.next(), 29.514929f, 9.878732f, "outer[4] LineTo");
  expectLineTo(iter.next(), 30.0f, 10.0f, "outer[5] LineTo");
  expectLineTo(iter.next(), 30.0f, 10.5f, "outer[6] LineTo");
  expectLineTo(iter.next(), 10.0f, 10.5f, "outer[7] LineTo");
  expectLineTo(iter.next(), 10.0f, 10.0f, "outer[8] LineTo");
  expectLineTo(iter.next(), 10.485071f, 9.878732f, "outer[9] LineTo (auto-close)");
  expectClose(iter.next(), "outer[10] Close");

  // Inner sub-path.
  expectMoveTo(iter.next(), 9.3596115f, 9.5f, "inner[0] MoveTo");
  expectLineTo(iter.next(), 30.640388f, 9.5f, "inner[1] LineTo");
  expectLineTo(iter.next(), 20.485071f, 50.121269f, "inner[2] LineTo");
  expectLineTo(iter.next(), 19.514929f, 50.121269f, "inner[3] LineTo");
  expectLineTo(iter.next(), 9.514929f, 10.121268f, "inner[4] LineTo");
  expectLineTo(iter.next(), 9.3596115f, 9.5f, "inner[5] LineTo (auto-close)");
  expectClose(iter.next(), "inner[6] Close");

  // No more segments.
  EXPECT_FALSE(iter.next().has_value()) << "should have no more segments after both sub-paths";
}

// Ported from Rust stroker.rs: cubic_1
// From skia/tests/StrokeTest.cpp — degenerate cubic (all four points identical)
// should produce no stroke output.
TEST(PathTest, StrokerDegenerateCubicProducesNoStroke) {
  tiny_skia::PathBuilder pb;
  pb.moveTo(51.0161362f, 1511.52478f);
  pb.cubicTo(51.0161362f, 1511.52478f, 51.0161362f, 1511.52478f, 51.0161362f, 1511.52478f);
  auto path = pb.finish();
  ASSERT_TRUE(path.has_value());

  tiny_skia::Stroke stroke;
  stroke.width = 0.394537568f;

  auto result = tiny_skia::PathStroker().stroke(*path, stroke, 1.0f);
  EXPECT_FALSE(result.has_value())
      << "stroking a degenerate cubic (all points identical) should return nullopt";
}

// Ported from Rust stroker.rs: cubic_2
// From skia/tests/StrokeTest.cpp — near-degenerate cubic with slightly different
// control points should produce a valid stroke.
TEST(PathTest, StrokerNearDegenerateCubicProducesStroke) {
  tiny_skia::PathBuilder pb;
  pb.moveTo(std::bit_cast<float>(std::uint32_t{0x424c1086}),
            std::bit_cast<float>(std::uint32_t{0x44bcf0cb}));
  pb.cubicTo(std::bit_cast<float>(std::uint32_t{0x424c107c}),
             std::bit_cast<float>(std::uint32_t{0x44bcf0cb}),
             std::bit_cast<float>(std::uint32_t{0x424c10c2}),
             std::bit_cast<float>(std::uint32_t{0x44bcf0cb}),
             std::bit_cast<float>(std::uint32_t{0x424c1119}),
             std::bit_cast<float>(std::uint32_t{0x44bcf0ca}));
  auto path = pb.finish();
  ASSERT_TRUE(path.has_value());

  tiny_skia::Stroke stroke;
  stroke.width = 0.394537568f;

  auto result = tiny_skia::PathStroker().stroke(*path, stroke, 1.0f);
  EXPECT_TRUE(result.has_value())
      << "stroking a near-degenerate cubic with slightly different points should succeed";
}

// Ported from Rust stroker.rs: big
// From skia/tests/StrokeTest.cpp (skbug.com/6491)
// Large stroke width can cause numerical instabilities.
TEST(PathTest, StrokerBigStrokeWidthDoesNotCrash) {
  tiny_skia::PathBuilder pb;
  pb.moveTo(std::bit_cast<float>(std::uint32_t{0x46380000}),
            std::bit_cast<float>(std::uint32_t{0xc6380000}));  // 11776, -11776
  pb.lineTo(std::bit_cast<float>(std::uint32_t{0x46a00000}),
            std::bit_cast<float>(std::uint32_t{0xc6a00000}));  // 20480, -20480
  pb.lineTo(std::bit_cast<float>(std::uint32_t{0x468c0000}),
            std::bit_cast<float>(std::uint32_t{0xc68c0000}));  // 17920, -17920
  pb.lineTo(std::bit_cast<float>(std::uint32_t{0x46100000}),
            std::bit_cast<float>(std::uint32_t{0xc6100000}));  // 9216, -9216
  pb.lineTo(std::bit_cast<float>(std::uint32_t{0x46380000}),
            std::bit_cast<float>(std::uint32_t{0xc6380000}));  // 11776, -11776
  pb.close();
  auto path = pb.finish();
  ASSERT_TRUE(path.has_value());

  tiny_skia::Stroke stroke;
  stroke.width = 1.49679073e+10f;

  auto result = tiny_skia::PathStroker().stroke(*path, stroke, 1.0f);
  EXPECT_TRUE(result.has_value()) << "very large stroke width should still produce a valid path";
}

// Ported from Rust stroker.rs: quad_stroker_one_off
// From skia/tests/StrokerTest.cpp
TEST(PathTest, StrokerQuadStrokerOneOff) {
  tiny_skia::PathBuilder pb;
  pb.moveTo(std::bit_cast<float>(std::uint32_t{0x43c99223}),
            std::bit_cast<float>(std::uint32_t{0x42b7417e}));
  pb.quadTo(std::bit_cast<float>(std::uint32_t{0x4285d839}),
            std::bit_cast<float>(std::uint32_t{0x43ed6645}),
            std::bit_cast<float>(std::uint32_t{0x43c941c8}),
            std::bit_cast<float>(std::uint32_t{0x42b3ace3}));
  auto path = pb.finish();
  ASSERT_TRUE(path.has_value());

  tiny_skia::Stroke stroke;
  stroke.width = 164.683548f;

  auto result = tiny_skia::PathStroker().stroke(*path, stroke, 1.0f);
  EXPECT_TRUE(result.has_value())
      << "quad stroker one-off test from Skia should produce a valid path";
}

// Ported from Rust stroker.rs: cubic_stroker_one_off
// From skia/tests/StrokerTest.cpp
TEST(PathTest, StrokerCubicStrokerOneOff) {
  tiny_skia::PathBuilder pb;
  pb.moveTo(std::bit_cast<float>(std::uint32_t{0x433f5370}),
            std::bit_cast<float>(std::uint32_t{0x43d1f4b3}));
  pb.cubicTo(std::bit_cast<float>(std::uint32_t{0x4331cb76}),
             std::bit_cast<float>(std::uint32_t{0x43ea3340}),
             std::bit_cast<float>(std::uint32_t{0x4388f498}),
             std::bit_cast<float>(std::uint32_t{0x42f7f08d}),
             std::bit_cast<float>(std::uint32_t{0x43f1cd32}),
             std::bit_cast<float>(std::uint32_t{0x42802ec1}));
  auto path = pb.finish();
  ASSERT_TRUE(path.has_value());

  tiny_skia::Stroke stroke;
  stroke.width = 42.835968f;

  auto result = tiny_skia::PathStroker().stroke(*path, stroke, 1.0f);
  EXPECT_TRUE(result.has_value())
      << "cubic stroker one-off test from Skia should produce a valid path";
}
