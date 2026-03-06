#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <vector>

#include "tiny_skia/EdgeClipper.h"
#include "tiny_skia/tests/test_utils/PathEdgeMatchers.h"

namespace {

using tiny_skia::tests::matchers::VerticalPathEdgeAtX;

std::vector<tiny_skia::PathEdge> CollectClippedEdges(const tiny_skia::Path& path,
                                                     bool canCullToTheRight) {
  const auto clip = tiny_skia::Rect::fromLTRB(0.0f, 0.0f, 10.0f, 10.0f).value();
  auto iter = tiny_skia::EdgeClipperIter(path, clip, canCullToTheRight);

  auto result = std::vector<tiny_skia::PathEdge>{};
  for (auto edges = iter.next(); edges.has_value(); edges = iter.next()) {
    for (const auto& edge : edges->span()) {
      result.push_back(edge);
    }
  }
  return result;
}

}  // namespace

TEST(EdgeClipperTest, ClipLinePassesThroughWhenFullyInsideClip) {
  tiny_skia::Path path;
  path.addVerb(tiny_skia::PathVerb::Move);
  path.addPoint({2.0f, 2.0f});
  path.addVerb(tiny_skia::PathVerb::Line);
  path.addPoint({5.0f, 5.0f});

  auto edges = CollectClippedEdges(path, true);
  ASSERT_GE(edges.size(), 1u);
  EXPECT_THAT(edges.front(), tiny_skia::tests::matchers::PathEdgeLineEq(2.0f, 2.0f, 5.0f, 5.0f));
  EXPECT_FLOAT_EQ(edges.front().points[0].x, 2.0f);
  EXPECT_FLOAT_EQ(edges.front().points[0].y, 2.0f);
  auto minX = 10.0f;
  auto maxX = 0.0f;
  auto minY = 10.0f;
  auto maxY = 0.0f;
  for (std::size_t i = 0; i + 1 < edges.size(); ++i) {
    EXPECT_FLOAT_EQ(edges[i].points[1].x, edges[i + 1].points[0].x);
    EXPECT_FLOAT_EQ(edges[i].points[1].y, edges[i + 1].points[0].y);
  }
  for (const auto& edge : edges) {
    for (std::size_t i = 0; i < 2; ++i) {
      minX = std::min(minX, edge.points[i].x);
      maxX = std::max(maxX, edge.points[i].x);
      minY = std::min(minY, edge.points[i].y);
      maxY = std::max(maxY, edge.points[i].y);
    }
  }
  EXPECT_FLOAT_EQ(minX, 2.0f);
  EXPECT_FLOAT_EQ(maxX, 5.0f);
  EXPECT_FLOAT_EQ(minY, 2.0f);
  EXPECT_FLOAT_EQ(maxY, 5.0f);
}

TEST(EdgeClipperTest, ClipLineCanCullToRightOrPreserveWhenDisabled) {
  tiny_skia::Path path;
  path.addVerb(tiny_skia::PathVerb::Move);
  path.addPoint({12.0f, 2.0f});
  path.addVerb(tiny_skia::PathVerb::Line);
  path.addPoint({20.0f, 5.0f});

  EXPECT_TRUE(CollectClippedEdges(path, true).empty());

  const auto preserved = CollectClippedEdges(path, false);
  ASSERT_GE(preserved.size(), 1u);
  EXPECT_THAT(preserved.front(), VerticalPathEdgeAtX(10.0f));
  auto minY = 10.0f;
  auto maxY = 0.0f;
  for (const auto& edge : preserved) {
    EXPECT_THAT(edge, VerticalPathEdgeAtX(10.0f));
    minY = std::min(minY, std::min(edge.points[0].y, edge.points[1].y));
    maxY = std::max(maxY, std::max(edge.points[0].y, edge.points[1].y));
    EXPECT_GE(edge.points[0].y, 2.0f);
    EXPECT_GE(edge.points[1].y, 2.0f);
    EXPECT_LE(edge.points[0].y, 5.0f);
    EXPECT_LE(edge.points[1].y, 5.0f);
  }
  for (std::size_t i = 0; i + 1 < preserved.size(); ++i) {
    EXPECT_FLOAT_EQ(preserved[i].points[1].x, preserved[i + 1].points[0].x);
    EXPECT_FLOAT_EQ(preserved[i].points[1].y, preserved[i + 1].points[0].y);
    EXPECT_FLOAT_EQ(preserved[i].points[0].x, 10.0f);
    EXPECT_FLOAT_EQ(preserved[i + 1].points[0].x, 10.0f);
  }
  EXPECT_FLOAT_EQ(minY, 2.0f);
  EXPECT_FLOAT_EQ(maxY, 5.0f);
  for (const auto& edge : preserved) {
    EXPECT_FLOAT_EQ(edge.points[0].x, 10.0f);
    EXPECT_FLOAT_EQ(edge.points[1].x, 10.0f);
    EXPECT_GE(edge.points[0].y, 2.0f);
    EXPECT_GE(edge.points[1].y, 2.0f);
    EXPECT_LE(edge.points[0].y, 5.0f);
    EXPECT_LE(edge.points[1].y, 5.0f);
  }
  for (std::size_t i = 0; i + 1 < preserved.size(); ++i) {
    EXPECT_FLOAT_EQ(preserved[i].points[1].x, preserved[i + 1].points[0].x);
    EXPECT_FLOAT_EQ(preserved[i].points[1].y, preserved[i + 1].points[0].y);
  }
}

TEST(EdgeClipperTest, ClipQuadFullyToLeftBecomesVerticalLine) {
  tiny_skia::Path path;
  path.addVerb(tiny_skia::PathVerb::Move);
  path.addPoint({-8.0f, 2.0f});
  path.addVerb(tiny_skia::PathVerb::Quad);
  path.addPoint({-6.0f, 5.0f});
  path.addPoint({-4.0f, 9.0f});

  auto edges = CollectClippedEdges(path, true);
  ASSERT_GE(edges.size(), 1u);
  auto minY = 10.0f;
  auto maxY = 0.0f;
  for (const auto& edge : edges) {
    EXPECT_THAT(edge, VerticalPathEdgeAtX(0.0f));
    minY = std::min(minY, std::min(edge.points[0].y, edge.points[1].y));
    maxY = std::max(maxY, std::max(edge.points[0].y, edge.points[1].y));
  }
  for (std::size_t i = 0; i + 1 < edges.size(); ++i) {
    EXPECT_FLOAT_EQ(edges[i].points[1].x, edges[i + 1].points[0].x);
    EXPECT_FLOAT_EQ(edges[i].points[1].y, edges[i + 1].points[0].y);
  }
  EXPECT_FLOAT_EQ(minY, 2.0f);
  EXPECT_FLOAT_EQ(maxY, 9.0f);
}

TEST(EdgeClipperTest, ClipQuadFullyToRightProducesRightVerticalLineWhenCullingDisabled) {
  tiny_skia::Path path;
  path.addVerb(tiny_skia::PathVerb::Move);
  path.addPoint({12.0f, 1.0f});
  path.addVerb(tiny_skia::PathVerb::Quad);
  path.addPoint({14.0f, 4.0f});
  path.addPoint({16.0f, 8.0f});

  const auto culled = CollectClippedEdges(path, true);
  EXPECT_TRUE(culled.empty());

  auto edges = CollectClippedEdges(path, false);
  ASSERT_GE(edges.size(), 1u);
  auto minY = 10.0f;
  auto maxY = 0.0f;
  for (const auto& edge : edges) {
    EXPECT_THAT(edge, VerticalPathEdgeAtX(10.0f));
    EXPECT_GE(edge.points[0].y, 1.0f);
    EXPECT_GE(edge.points[1].y, 1.0f);
    EXPECT_LE(edge.points[0].y, 8.0f);
    EXPECT_LE(edge.points[1].y, 8.0f);
    minY = std::min(minY, std::min(edge.points[0].y, edge.points[1].y));
    maxY = std::max(maxY, std::max(edge.points[0].y, edge.points[1].y));
  }
  for (std::size_t i = 0; i + 1 < edges.size(); ++i) {
    EXPECT_FLOAT_EQ(edges[i].points[1].x, edges[i + 1].points[0].x);
    EXPECT_FLOAT_EQ(edges[i].points[1].y, edges[i + 1].points[0].y);
  }
  EXPECT_FLOAT_EQ(minY, 1.0f);
  EXPECT_FLOAT_EQ(maxY, 8.0f);
}

TEST(EdgeClipperTest, ClipCubicPartiallyInsideProducesCubicAndBoundaryLines) {
  tiny_skia::Path path;
  path.addVerb(tiny_skia::PathVerb::Move);
  path.addPoint({-6.0f, 2.0f});
  path.addVerb(tiny_skia::PathVerb::Cubic);
  path.addPoint({2.0f, 2.0f});
  path.addPoint({8.0f, 8.0f});
  path.addPoint({12.0f, 8.0f});

  auto edges = CollectClippedEdges(path, true);
  ASSERT_GE(edges.size(), 1u);
  bool hasLeftBoundary = false;
  for (const auto& edge : edges) {
    if (edge.type == tiny_skia::PathEdgeType::LineTo && edge.points[0].x == 0.0f &&
        edge.points[1].x == 0.0f) {
      hasLeftBoundary = true;
      break;
    }
  }
  EXPECT_TRUE(hasLeftBoundary);
}

TEST(EdgeClipperTest, ClipVeryLargeCubicFallsBackToLineClipping) {
  tiny_skia::Path path;
  path.addVerb(tiny_skia::PathVerb::Move);
  path.addPoint({-8388608.0f, 2.0f});
  path.addVerb(tiny_skia::PathVerb::Cubic);
  path.addPoint({8388608.0f, 2.0f});
  path.addPoint({8388608.0f, 8.0f});
  path.addPoint({-8388608.0f, 8.0f});

  auto edges = CollectClippedEdges(path, true);
  ASSERT_GE(edges.size(), 1u);
  for (const auto& edge : edges) {
    EXPECT_EQ(edge.type, tiny_skia::PathEdgeType::LineTo);
    EXPECT_LE(edge.points[0].x, 10.0f);
    EXPECT_GE(edge.points[0].x, 0.0f);
    EXPECT_LE(edge.points[1].x, 10.0f);
    EXPECT_GE(edge.points[1].x, 0.0f);
  }
}
