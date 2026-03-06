#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <span>

#include "tiny_skia/Edge.h"

namespace {
using testing::Field;
using testing::Optional;
}  // namespace

TEST(EdgeLineTest, LineEdgeCreateRejectsHorizontalRun) {
  const tiny_skia::Point p0{1.0f, 1.0f};
  const tiny_skia::Point p1{5.0f, 1.0f};

  EXPECT_THAT(tiny_skia::LineEdge::create(p0, p1, 0), testing::Eq(std::nullopt));
}

TEST(EdgeLineTest, LineEdgeCreateAssignsWindingAndBounds) {
  const tiny_skia::Point p0{0.0f, 0.0f};
  const tiny_skia::Point p1{4.0f, 2.0f};
  const auto edgeOpt = tiny_skia::LineEdge::create(p0, p1, 0);

  ASSERT_THAT(edgeOpt, Optional(Field(&tiny_skia::LineEdge::winding, 1)));
  const auto edge = *edgeOpt;
  EXPECT_FALSE(edge.isVertical());
  EXPECT_LT(edge.firstY, edge.lastY);
}

TEST(EdgeLineTest, LineEdgeCreateFlipsWindingForDescendingY) {
  const tiny_skia::Point p0{0.0f, 2.0f};
  const tiny_skia::Point p1{4.0f, 0.0f};
  EXPECT_THAT(tiny_skia::LineEdge::create(p0, p1, 0),
              Optional(Field(&tiny_skia::LineEdge::winding, -1)));
}

TEST(EdgeLineTest, LineEdgeIsVertical) {
  const tiny_skia::Point p0{2.0f, 0.0f};
  const tiny_skia::Point p1{2.0f, 1.0f};
  const auto edgeOpt = tiny_skia::LineEdge::create(p0, p1, 0);

  ASSERT_THAT(edgeOpt, Optional(testing::_));
  const auto edge = *edgeOpt;
  EXPECT_TRUE(edge.isVertical());
}

TEST(EdgeLineTest, EdgeAsLineReturnsUnderlyingLine) {
  const tiny_skia::Point p0{1.0f, 0.0f};
  const tiny_skia::Point p1{2.0f, 3.0f};
  const auto edgeOpt = tiny_skia::LineEdge::create(p0, p1, 0);

  ASSERT_THAT(edgeOpt, Optional(testing::_));
  const tiny_skia::Edge edge(*edgeOpt);

  const auto& line = edge.asLine();
  EXPECT_GE(line.lastY, line.firstY);
}
