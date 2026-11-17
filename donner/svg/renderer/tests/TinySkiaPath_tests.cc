#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "tiny_skia/Mask.h"
#include "tiny_skia/Path.h"
#include "tiny_skia/PathBuilder.h"
#include "tiny_skia/Stroke.h"

using ::testing::Optional;

TEST(PathTest, ConvexClassificationAcceptsExplicitClosingVertex) {
  tiny_skia::PathBuilder builder;
  builder.moveTo(2.0f, 2.0f);
  builder.lineTo(8.0f, 2.0f);
  builder.lineTo(8.0f, 8.0f);
  builder.lineTo(2.0f, 8.0f);
  builder.lineTo(2.0f, 2.0f);
  builder.close();
  const auto path = builder.finish();
  ASSERT_THAT(path, Optional(testing::_));
  EXPECT_THAT(path->isConvex(), testing::IsTrue());
}

TEST(PathTest, ConvexClassificationAcceptsClosedCurveControlPolygon) {
  constexpr float kNear = 3.34314575f;
  constexpr float kFar = 6.65685425f;
  tiny_skia::PathBuilder builder;
  builder.moveTo(8.0f, 5.0f);
  builder.cubicTo(8.0f, kFar, kFar, 8.0f, 5.0f, 8.0f);
  builder.cubicTo(kNear, 8.0f, 2.0f, kFar, 2.0f, 5.0f);
  builder.cubicTo(2.0f, kNear, kNear, 2.0f, 5.0f, 2.0f);
  builder.cubicTo(kFar, 2.0f, 8.0f, kNear, 8.0f, 5.0f);
  builder.close();
  const auto path = builder.finish();
  ASSERT_THAT(path, Optional(testing::_));
  EXPECT_THAT(path->isConvex(), testing::IsTrue());
}

TEST(PathTest, ConvexClassificationAcceptsCoincidentCurveControlPoints) {
  constexpr float kNear = 3.34314575f;
  constexpr float kFar = 6.65685425f;
  tiny_skia::PathBuilder builder;
  builder.moveTo(8.0f, 5.0f);
  // A control point on its own endpoint adds a zero-length control edge, not a revisit.
  builder.cubicTo(8.0f, 5.0f, kFar, 8.0f, 5.0f, 8.0f);
  builder.cubicTo(kNear, 8.0f, 2.0f, kFar, 2.0f, 5.0f);
  builder.cubicTo(2.0f, kNear, kNear, 2.0f, 5.0f, 2.0f);
  builder.cubicTo(kFar, 2.0f, 8.0f, 5.0f, 8.0f, 5.0f);
  builder.close();
  const auto path = builder.finish();
  ASSERT_THAT(path, Optional(testing::_));
  EXPECT_THAT(path->isConvex(), testing::IsTrue());
}

TEST(PathTest, ConvexClassificationRejectsRepeatedContour) {
  tiny_skia::PathBuilder builder;
  builder.moveTo(2.0f, 2.0f);
  for (int lap = 0; lap < 2; ++lap) {
    builder.lineTo(8.0f, 2.0f);
    builder.lineTo(8.0f, 8.0f);
    builder.lineTo(2.0f, 8.0f);
    if (lap == 0) {
      builder.lineTo(2.0f, 2.0f);
    }
  }
  builder.close();
  const auto path = builder.finish();
  ASSERT_THAT(path, Optional(testing::_));
  EXPECT_THAT(path->isConvex(), testing::IsFalse());
  const auto rectangle = tiny_skia::Rect::fromLTRB(2.0f, 2.0f, 8.0f, 8.0f);
  ASSERT_THAT(rectangle, Optional(testing::_));
  EXPECT_THAT(tiny_skia::Path::fromRect(*rectangle).isConvex(), testing::IsTrue());
}

TEST(PathTest, ConvexClassificationRejectsRepeatedExplicitlyClosedContour) {
  tiny_skia::PathBuilder builder;
  builder.moveTo(2.0f, 2.0f);
  for (int lap = 0; lap < 2; ++lap) {
    builder.lineTo(8.0f, 2.0f);
    builder.lineTo(8.0f, 8.0f);
    builder.lineTo(2.0f, 8.0f);
    builder.lineTo(2.0f, 2.0f);
  }
  builder.close();
  const auto path = builder.finish();
  ASSERT_THAT(path, Optional(testing::_));
  EXPECT_THAT(path->isConvex(), testing::IsFalse());
}

TEST(MaskTest, RepeatedContourRespectsWindingRule) {
  tiny_skia::PathBuilder builder;
  builder.moveTo(2.0f, 2.0f);
  for (int lap = 0; lap < 2; ++lap) {
    builder.lineTo(8.0f, 2.0f);
    builder.lineTo(8.0f, 8.0f);
    builder.lineTo(2.0f, 8.0f);
    if (lap == 0) {
      builder.lineTo(2.0f, 2.0f);
    }
  }
  builder.close();
  const auto path = builder.finish();
  ASSERT_THAT(path, Optional(testing::_));
  for (const bool antiAlias : {false, true}) {
    for (const auto rule : {tiny_skia::FillRule::Winding, tiny_skia::FillRule::EvenOdd}) {
      SCOPED_TRACE(antiAlias);
      SCOPED_TRACE(static_cast<int>(rule));
      auto mask = tiny_skia::Mask::fromSize(10, 10);
      ASSERT_THAT(mask, Optional(testing::_));
      mask->fillPath(*path, rule, antiAlias, tiny_skia::Transform::identity());
      std::vector<std::uint8_t> expected(100, 0);
      if (rule == tiny_skia::FillRule::Winding) {
        for (std::size_t y = 2; y < 8; ++y) {
          for (std::size_t x = 2; x < 8; ++x) {
            expected[y * 10 + x] = 255;
          }
        }
      }
      EXPECT_THAT(mask->data(), testing::ElementsAreArray(expected));
    }
  }
}

TEST(PathTest, ConvexClassificationRejectsOverlapsAndTouches) {
  const std::vector<std::vector<tiny_skia::Point>> contours = {
      {{0, 0}, {4, 0}, {4, 4}, {0, 4}, {0, 2}, {4, 2}, {4, 6}, {0, 6}},
      {{0, 0}, {4, 0}, {2, 4}, {0, 0}, {3, 1}, {2, 3}},
  };
  for (const auto& points : contours) {
    SCOPED_TRACE(testing::PrintToString(points));
    tiny_skia::PathBuilder builder;
    builder.moveTo(points.front().x, points.front().y);
    for (std::size_t i = 1; i < points.size(); ++i) {
      builder.lineTo(points[i].x, points[i].y);
    }
    builder.close();
    const auto path = builder.finish();
    ASSERT_THAT(path, Optional(testing::_));
    EXPECT_THAT(path->isConvex(), testing::IsFalse());
  }
}

TEST(PathTest, ConvexClassificationRequiresFiniteTurns) {
  for (const float scale : {1.0f, std::numeric_limits<float>::max() / 4.0f}) {
    SCOPED_TRACE(scale);
    tiny_skia::PathBuilder builder;
    builder.moveTo(-scale, -scale);
    builder.lineTo(scale, -scale);
    builder.lineTo(0, scale);
    builder.close();
    const auto path = builder.finish();
    ASSERT_THAT(path, Optional(testing::_));
    EXPECT_THAT(path->isConvex(), testing::Eq(scale == 1.0f));
  }
}

namespace {

/// A circle of four cubics, radius 40 around (50, 50).
tiny_skia::Path CubicCircle() {
  constexpr float kKappa = 0.5522847f;
  constexpr float kCenter = 50.0f;
  constexpr float kRadius = 40.0f;
  constexpr float kHandle = kKappa * kRadius;
  tiny_skia::PathBuilder builder;
  builder.moveTo(kCenter + kRadius, kCenter);
  builder.cubicTo(kCenter + kRadius, kCenter + kHandle, kCenter + kHandle, kCenter + kRadius,
                  kCenter, kCenter + kRadius);
  builder.cubicTo(kCenter - kHandle, kCenter + kRadius, kCenter - kRadius, kCenter + kHandle,
                  kCenter - kRadius, kCenter);
  builder.cubicTo(kCenter - kRadius, kCenter - kHandle, kCenter - kHandle, kCenter - kRadius,
                  kCenter, kCenter - kRadius);
  builder.cubicTo(kCenter + kHandle, kCenter - kRadius, kCenter + kRadius, kCenter - kHandle,
                  kCenter + kRadius, kCenter);
  builder.close();
  return *builder.finish();
}

}  // namespace

TEST(PathTest, StrokeStopsAtTheOutputLimit) {
  // At a resolution scale of 100,000 the stroker emits about 16,000 outline points for this
  // circle. A limit below that stops the expansion and produces no outline.
  const tiny_skia::Path circle = CubicCircle();
  tiny_skia::Stroke stroke;
  stroke.width = 2.0f;

  tiny_skia::StrokeOutputLimit tight{.maxPoints = 4096};
  EXPECT_THAT(circle.stroke(stroke, 100000.0f, &tight), testing::Eq(std::nullopt));
  EXPECT_TRUE(tight.exceeded);

  tiny_skia::StrokeOutputLimit ample{.maxPoints = std::size_t{1} << 20};
  const std::optional<tiny_skia::Path> outline = circle.stroke(stroke, 100000.0f, &ample);
  ASSERT_THAT(outline, Optional(testing::_));
  EXPECT_FALSE(ample.exceeded);
  EXPECT_THAT(outline->points().size(),
              testing::AllOf(testing::Gt(4096u), testing::Le(ample.maxPoints)));

  // Without a limit the stroke is unchanged.
  const std::optional<tiny_skia::Path> unlimited = circle.stroke(stroke, 100000.0f);
  ASSERT_THAT(unlimited, Optional(testing::_));
  EXPECT_THAT(unlimited->points().size(), testing::Eq(outline->points().size()));
}

TEST(PathTest, StrokeOutputLimitHoldsForTheFinishedOutline) {
  // The last contour's caps, its closing join, and the inner side appended when a contour is
  // finished are all emitted after the stroker's last check inside the segment loop. The limit
  // still holds for the outline returned.
  tiny_skia::Stroke roundCaps;
  roundCaps.width = 2.0f;
  roundCaps.lineCap = tiny_skia::LineCap::Round;
  tiny_skia::PathBuilder open;
  open.moveTo(0.0f, 0.0f);
  open.lineTo(10.0f, 0.0f);
  const tiny_skia::Path line = *open.finish();

  tiny_skia::Stroke miterJoins;
  miterJoins.width = 2.0f;
  tiny_skia::PathBuilder closed;
  closed.moveTo(0.0f, 0.0f);
  closed.lineTo(10.0f, 0.0f);
  closed.lineTo(10.0f, 10.0f);
  closed.close();
  const tiny_skia::Path triangle = *closed.finish();

  for (const auto& [path, stroke] : {std::pair{line, roundCaps}, std::pair{triangle, miterJoins}}) {
    const std::optional<tiny_skia::Path> full = path.stroke(stroke, 1.0f);
    ASSERT_THAT(full, Optional(testing::_));
    const std::size_t points = full->points().size();
    tiny_skia::StrokeOutputLimit tight{.maxPoints = points - 1};
    EXPECT_THAT(path.stroke(stroke, 1.0f, &tight), testing::Eq(std::nullopt));
    EXPECT_TRUE(tight.exceeded);
    tiny_skia::StrokeOutputLimit exact{.maxPoints = points};
    EXPECT_THAT(path.stroke(stroke, 1.0f, &exact), Optional(testing::_));
    EXPECT_FALSE(exact.exceeded);
    EXPECT_EQ(exact.emittedPoints, points);
  }

  tiny_skia::StrokeOutputLimit tiny{.maxPoints = 4};
  EXPECT_THAT(line.stroke(roundCaps, 1.0f, &tiny), testing::Eq(std::nullopt));
  EXPECT_TRUE(tiny.exceeded);
}
