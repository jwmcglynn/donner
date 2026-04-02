#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <limits>

#include "tiny_skia/EdgeBuilder.h"
#include "tiny_skia/Geom.h"
#include "tiny_skia/tests/test_utils/GeomMatchers.h"
#include "tiny_skia/tests/test_utils/PathEdgeMatchers.h"

namespace {

using tiny_skia::tests::matchers::OptionalPathEdgeLineEq;
using tiny_skia::tests::matchers::ScreenIntRectEq;

TEST(EdgeBuilderTest, BuildEdgesRejectsEmptyPath) {
  tiny_skia::Path path;
  const auto edges = tiny_skia::BasicEdgeBuilder::buildEdges(path, nullptr, 0);
  EXPECT_FALSE(edges.has_value());
}

TEST(EdgeBuilderTest, BuildEdgesBuildsSimpleTriangle) {
  tiny_skia::Path path;
  path.addVerb(tiny_skia::PathVerb::Move);
  path.addPoint({0.0f, 0.0f});
  path.addVerb(tiny_skia::PathVerb::Line);
  path.addPoint({10.0f, 10.0f});
  path.addVerb(tiny_skia::PathVerb::Close);

  const auto edges = tiny_skia::BasicEdgeBuilder::buildEdges(path, nullptr, 0);
  ASSERT_TRUE(edges.has_value());
  ASSERT_GE(edges->size(), 2u);
  EXPECT_TRUE(
      std::all_of(edges->begin(), edges->end(), [](const auto& edge) { return edge.isLine(); }));
}

TEST(EdgeBuilderTest, BuildEdgesBuildsMixedEdgesForMonotonicQuad) {
  tiny_skia::Path path;
  path.addVerb(tiny_skia::PathVerb::Move);
  path.addPoint({0.0f, 0.0f});
  path.addVerb(tiny_skia::PathVerb::Quad);
  path.addPoint({10.0f, 10.0f});
  path.addPoint({20.0f, 0.0f});
  path.addVerb(tiny_skia::PathVerb::Line);
  path.addPoint({20.0f, 10.0f});
  path.addVerb(tiny_skia::PathVerb::Close);

  const auto edges = tiny_skia::BasicEdgeBuilder::buildEdges(path, nullptr, 0);
  ASSERT_TRUE(edges.has_value());
  EXPECT_GE(edges->size(), 2u);
  const auto hasQuadratic = std::any_of(edges->begin(), edges->end(),
                                        [](const auto& edge) { return edge.isQuadratic(); });
  const auto hasLine =
      std::any_of(edges->begin(), edges->end(), [](const auto& edge) { return edge.isLine(); });
  EXPECT_TRUE(hasQuadratic);
  EXPECT_TRUE(hasLine);
}

TEST(EdgeBuilderTest, BuildEdgesAutoClosesTrailingOpenContour) {
  tiny_skia::Path path;
  path.addVerb(tiny_skia::PathVerb::Move);
  path.addPoint({2.0f, 1.0f});
  path.addVerb(tiny_skia::PathVerb::Line);
  path.addPoint({7.0f, 3.0f});
  path.addVerb(tiny_skia::PathVerb::Line);
  path.addPoint({9.0f, 5.0f});

  const auto edges = tiny_skia::BasicEdgeBuilder::buildEdges(path, nullptr, 0);
  ASSERT_TRUE(edges.has_value());
  ASSERT_GE(edges->size(), 2u);
  EXPECT_TRUE(edges->front().isLine());
  EXPECT_TRUE(edges->back().isLine());
}

TEST(EdgeBuilderTest, BuildEdgesRejectsMalformedPath) {
  tiny_skia::Path path;
  path.addVerb(tiny_skia::PathVerb::Move);
  path.addPoint({0.0f, 0.0f});
  path.addVerb(tiny_skia::PathVerb::Line);

  const auto edges = tiny_skia::BasicEdgeBuilder::buildEdges(path, nullptr, 0);
  EXPECT_FALSE(edges.has_value());
}

TEST(EdgeBuilderTest, BuildEdgesWithClipRejectsPathCompletelyOutsideClip) {
  tiny_skia::Path path;
  path.addVerb(tiny_skia::PathVerb::Move);
  path.addPoint({1.0f, 20.0f});
  path.addVerb(tiny_skia::PathVerb::Line);
  path.addPoint({9.0f, 25.0f});

  const auto clipRect = tiny_skia::ScreenIntRect::fromXYWH(0, 0, 10, 10).value();
  const auto shiftedClip = tiny_skia::ShiftedIntRect::create(clipRect, 0).value();

  const auto edges = tiny_skia::BasicEdgeBuilder::buildEdges(path, nullptr, 0);
  ASSERT_TRUE(edges.has_value());
  const auto clippedEdges = tiny_skia::BasicEdgeBuilder::buildEdges(path, &shiftedClip, 0);
  EXPECT_FALSE(clippedEdges.has_value());
}

TEST(EdgeBuilderTest, BuildEdgesWithClipChangesPartiallyOutsidePath) {
  tiny_skia::Path path;
  path.addVerb(tiny_skia::PathVerb::Move);
  path.addPoint({-5.0f, 5.0f});
  path.addVerb(tiny_skia::PathVerb::Line);
  path.addPoint({20.0f, 5.0f});
  path.addVerb(tiny_skia::PathVerb::Line);
  path.addPoint({20.0f, 20.0f});
  path.addVerb(tiny_skia::PathVerb::Close);

  const auto clipRect = tiny_skia::ScreenIntRect::fromXYWH(0, 0, 10, 10).value();
  const auto shiftedClip = tiny_skia::ShiftedIntRect::create(clipRect, 0).value();

  const auto edges = tiny_skia::BasicEdgeBuilder::buildEdges(path, nullptr, 0);
  ASSERT_TRUE(edges.has_value());
  const auto clippedEdges = tiny_skia::BasicEdgeBuilder::buildEdges(path, &shiftedClip, 0);
  ASSERT_TRUE(clippedEdges.has_value());
  EXPECT_NE(edges->size(), clippedEdges->size());
}

TEST(EdgeBuilderTest, BuildEdgesMergesContiguousVerticalSegments) {
  tiny_skia::Path path;
  path.addVerb(tiny_skia::PathVerb::Move);
  path.addPoint({2.0f, 0.0f});
  path.addVerb(tiny_skia::PathVerb::Line);
  path.addPoint({2.0f, 5.0f});
  path.addVerb(tiny_skia::PathVerb::Line);
  path.addPoint({2.0f, 10.0f});
  path.addVerb(tiny_skia::PathVerb::Line);
  path.addPoint({3.0f, 10.0f});

  auto builder = tiny_skia::BasicEdgeBuilder::newBuilder(0);
  EXPECT_TRUE(builder.build(path, nullptr, false));
  EXPECT_EQ(builder.edges().size(), 2u);
  EXPECT_TRUE(builder.edges().front().isLine());
  const auto& mergedEdge = builder.edges().front().asLine();
  EXPECT_EQ(mergedEdge.firstY, 0);
  EXPECT_EQ(mergedEdge.lastY, 9);
}

TEST(EdgeBuilderTest, BuildEdgesCancelsOppositeWindingVerticalSegments) {
  tiny_skia::Path path;
  path.addVerb(tiny_skia::PathVerb::Move);
  path.addPoint({3.0f, 0.0f});
  path.addVerb(tiny_skia::PathVerb::Line);
  path.addPoint({3.0f, 5.0f});
  path.addVerb(tiny_skia::PathVerb::Line);
  path.addPoint({3.0f, 0.0f});

  auto edges = tiny_skia::BasicEdgeBuilder::buildEdges(path, nullptr, 0);
  EXPECT_FALSE(edges.has_value());
}

TEST(EdgeBuilderTest, BuildEdgesRejectsNonFiniteInputs) {
  tiny_skia::Path path;
  path.addVerb(tiny_skia::PathVerb::Move);
  path.addPoint({0.0f, 0.0f});
  path.addVerb(tiny_skia::PathVerb::Line);
  path.addPoint({std::numeric_limits<float>::quiet_NaN(), 10.0f});

  const auto edges = tiny_skia::BasicEdgeBuilder::buildEdges(path, nullptr, 0);
  EXPECT_FALSE(edges.has_value());
}

TEST(EdgeBuilderTest, PathIterClosesOpenContourBeforeMoveVerb) {
  tiny_skia::Path path;
  path.addVerb(tiny_skia::PathVerb::Move);
  path.addPoint({0.0f, 0.0f});
  path.addVerb(tiny_skia::PathVerb::Line);
  path.addPoint({10.0f, 0.0f});
  path.addVerb(tiny_skia::PathVerb::Move);
  path.addPoint({20.0f, 0.0f});
  path.addVerb(tiny_skia::PathVerb::Line);
  path.addPoint({30.0f, 10.0f});

  auto iter = tiny_skia::pathIter(path);
  auto first = iter.next();
  auto second = iter.next();
  auto third = iter.next();

  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  ASSERT_TRUE(third.has_value());

  EXPECT_THAT(first, OptionalPathEdgeLineEq(0.0f, 0.0f, 10.0f, 0.0f));
  EXPECT_THAT(second, OptionalPathEdgeLineEq(10.0f, 0.0f, 0.0f, 0.0f));
  EXPECT_THAT(third, OptionalPathEdgeLineEq(20.0f, 0.0f, 30.0f, 10.0f));
}

TEST(EdgeBuilderTest, ShiftedIntRectCreateRoundTrips) {
  const auto source = tiny_skia::ScreenIntRect::fromXYWH(1, 2, 10, 20).value();
  const auto shifted = tiny_skia::ShiftedIntRect::create(source, 2).value();

  const auto recovered = shifted.recover();
  EXPECT_THAT(shifted.shifted(), ScreenIntRectEq(4u, 8u, 40u, 80u));
  EXPECT_THAT(recovered, ScreenIntRectEq(source.x(), source.y(), source.width(), source.height()));
}

TEST(EdgeBuilderTest, ShiftedIntRectCreateRejectsBadShifts) {
  const auto source = tiny_skia::ScreenIntRect::fromXYWH(1, 2, 10, 20).value();
  EXPECT_FALSE(tiny_skia::ShiftedIntRect::create(source, -1).has_value());
  EXPECT_FALSE(tiny_skia::ShiftedIntRect::create(source, 31).has_value());
}

}  // namespace
