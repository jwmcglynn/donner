#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <span>

#include "tiny_skia/Edge.h"

namespace {
using testing::Field;
using testing::Gt;
using testing::Optional;
}  // namespace

TEST(EdgeQuadraticTest, QuadraticEdgeCreateRejectsBadInputs) {
  const std::array<tiny_skia::Point, 2> badQuad{{{0.0f, 0.0f}, {1.0f, 1.0f}}};
  EXPECT_THAT(tiny_skia::QuadraticEdge::create(std::span{badQuad.data(), badQuad.size()}, 0),
              testing::Eq(std::nullopt));
}

TEST(EdgeQuadraticTest, QuadraticEdgeCreateBasic) {
  const std::array<tiny_skia::Point, 3> quad{{{0.0f, 0.0f}, {2.0f, 4.0f}, {4.0f, 6.0f}}};

  const auto edgeOpt = tiny_skia::QuadraticEdge::create(std::span{quad.data(), quad.size()}, 0);
  ASSERT_THAT(edgeOpt, Optional(Field(&tiny_skia::QuadraticEdge::curveCount, Gt(0))));
  const auto& edge = *edgeOpt;
  EXPECT_TRUE(edge.line.firstY < edge.line.lastY);
  EXPECT_NE(edge.qLastX, edge.qx);
  EXPECT_NE(edge.qLastY, edge.qy);
}

TEST(EdgeQuadraticTest, QuadraticEdgeCreateDescendingYFlipsWinding) {
  const std::array<tiny_skia::Point, 3> quad{{{0.0f, 5.0f}, {2.0f, 3.0f}, {4.0f, 1.0f}}};

  const auto edgeOpt = tiny_skia::QuadraticEdge::create(std::span{quad.data(), quad.size()}, 0);
  EXPECT_THAT(edgeOpt, Optional(Field(&tiny_skia::QuadraticEdge::line,
                                      Field(&tiny_skia::LineEdge::winding, -1))));
}

TEST(EdgeQuadraticTest, QuadraticEdgeUpdateCanAdvance) {
  const std::array<tiny_skia::Point, 3> quad{{{0.0f, 0.0f}, {1.0f, 2.0f}, {2.0f, 4.0f}}};

  auto edgeOpt = tiny_skia::QuadraticEdge::create(std::span{quad.data(), quad.size()}, 0);
  ASSERT_THAT(edgeOpt, Optional(testing::_));

  auto edge = *edgeOpt;
  const auto xBefore = edge.qx;
  const auto yBefore = edge.qy;

  (void)edge.update();

  EXPECT_NE(edge.qx, xBefore);
  EXPECT_NE(edge.qy, yBefore);
}
