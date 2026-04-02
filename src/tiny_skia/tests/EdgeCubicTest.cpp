#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <span>

#include "tiny_skia/Edge.h"

namespace {
using testing::Field;
using testing::Lt;
using testing::Optional;
}  // namespace

TEST(EdgeCubicTest, CubicEdgeCreateRejectsBadInputs) {
  const std::array<tiny_skia::Point, 3> badCubic{{{0.0f, 0.0f}, {1.0f, 1.0f}, {2.0f, 2.0f}}};
  EXPECT_THAT(tiny_skia::CubicEdge::create(std::span{badCubic.data(), badCubic.size()}, 0),
              testing::Eq(std::nullopt));
}

TEST(EdgeCubicTest, CubicEdgeCreateBasic) {
  const std::array<tiny_skia::Point, 4> cubic{
      {{0.0f, 0.0f}, {2.0f, 0.0f}, {4.0f, 6.0f}, {6.0f, 6.0f}}};

  const auto edgeOpt = tiny_skia::CubicEdge::create(std::span{cubic.data(), cubic.size()}, 0);
  ASSERT_THAT(edgeOpt, Optional(Field(&tiny_skia::CubicEdge::curveCount, Lt(0))));
  const auto& edge = *edgeOpt;
  EXPECT_NE(edge.cLastX, edge.cx);
  EXPECT_NE(edge.cLastY, edge.cy);
}

TEST(EdgeCubicTest, CubicEdgeCreateDescendingYFlipsWindingAndUpdates) {
  const std::array<tiny_skia::Point, 4> cubic{
      {{0.0f, 8.0f}, {2.0f, 6.0f}, {4.0f, 4.0f}, {6.0f, 2.0f}}};

  auto edgeOpt = tiny_skia::CubicEdge::create(std::span{cubic.data(), cubic.size()}, 0);
  ASSERT_THAT(edgeOpt, Optional(testing::_));
  auto edge = *edgeOpt;
  EXPECT_THAT(edge.line, Field(&tiny_skia::LineEdge::winding, -1));

  const auto xBefore = edge.cx;
  const auto yBefore = edge.cy;
  (void)edge.update();
  EXPECT_NE(edge.cx, xBefore);
  EXPECT_NE(edge.cy, yBefore);
}
