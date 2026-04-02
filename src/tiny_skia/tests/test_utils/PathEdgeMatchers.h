#pragma once

#include <gmock/gmock.h>

#include <optional>

#include "tiny_skia/EdgeBuilder.h"
#include "tiny_skia/Point.h"

namespace tiny_skia::tests::matchers {

inline ::testing::Matcher<const tiny_skia::Point&> PointEq(float x, float y) {
  using ::testing::AllOf;
  using ::testing::Eq;
  using ::testing::Field;
  using ::testing::FloatEq;
  using ::testing::ResultOf;

  return AllOf(Field("x", &tiny_skia::Point::x, FloatEq(x)),
               Field("y", &tiny_skia::Point::y, FloatEq(y)),
               ResultOf(
                   "is finite", [](const tiny_skia::Point& p) { return p.x == p.x && p.y == p.y; },
                   Eq(true)));
}

inline ::testing::Matcher<const tiny_skia::PathEdge&> PathEdgeLineEq(float x0, float y0, float x1,
                                                                     float y1) {
  using ::testing::AllOf;
  using ::testing::Eq;
  using ::testing::Field;
  using ::testing::ResultOf;

  return AllOf(
      Field("type", &tiny_skia::PathEdge::type, Eq(tiny_skia::PathEdgeType::LineTo)),
      ResultOf(
          "p0", [](const tiny_skia::PathEdge& edge) { return edge.points[0]; }, PointEq(x0, y0)),
      ResultOf(
          "p1", [](const tiny_skia::PathEdge& edge) { return edge.points[1]; }, PointEq(x1, y1)));
}

inline ::testing::Matcher<const std::optional<tiny_skia::PathEdge>&> OptionalPathEdgeLineEq(
    float x0, float y0, float x1, float y1) {
  return ::testing::Optional(PathEdgeLineEq(x0, y0, x1, y1));
}

inline ::testing::Matcher<const tiny_skia::PathEdge&> VerticalPathEdgeAtX(float x) {
  using ::testing::AllOf;
  using ::testing::FloatEq;
  using ::testing::ResultOf;

  return AllOf(
      ResultOf(
          "p0.x", [](const tiny_skia::PathEdge& edge) { return edge.points[0].x; }, FloatEq(x)),
      ResultOf(
          "p1.x", [](const tiny_skia::PathEdge& edge) { return edge.points[1].x; }, FloatEq(x)),
      ResultOf(
          "line type",
          [](const tiny_skia::PathEdge& edge) {
            return edge.type == tiny_skia::PathEdgeType::LineTo;
          },
          ::testing::Eq(true)));
}

}  // namespace tiny_skia::tests::matchers
