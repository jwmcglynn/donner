#include "donner/svg/renderer/FilterGraphExecutor.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <utility>

namespace donner::svg {
namespace {

using Pixel = std::array<std::uint8_t, 4>;

void SetPixel(tiny_skia::Pixmap& pixmap, int x, int y, Pixel pixel) {
  auto data = pixmap.data();
  const std::size_t index =
      (static_cast<std::size_t>(y) * pixmap.width() + static_cast<std::size_t>(x)) * 4u;
  data[index + 0] = pixel[0];
  data[index + 1] = pixel[1];
  data[index + 2] = pixel[2];
  data[index + 3] = pixel[3];
}

Pixel GetPixel(const tiny_skia::Pixmap& pixmap, int x, int y) {
  const auto data = pixmap.data();
  const std::size_t index =
      (static_cast<std::size_t>(y) * pixmap.width() + static_cast<std::size_t>(x)) * 4u;
  return Pixel{data[index + 0], data[index + 1], data[index + 2], data[index + 3]};
}

tiny_skia::Pixmap CreateBlurredDotPixmap(const Transformd& filterTransform) {
  auto maybePixmap = tiny_skia::Pixmap::fromSize(32, 32);
  EXPECT_TRUE(maybePixmap.has_value());
  tiny_skia::Pixmap pixmap = std::move(*maybePixmap);

  SetPixel(pixmap, 16, 16, Pixel{255, 255, 255, 255});

  components::FilterGraph graph;
  components::FilterNode node;
  node.inputs.push_back(components::FilterInput{});
  node.primitive = components::filter_primitive::GaussianBlur{
      .stdDeviationX = 4.0,
      .stdDeviationY = 4.0,
  };
  graph.nodes.push_back(std::move(node));

  ApplyFilterGraphToPixmap(pixmap, graph, filterTransform, std::nullopt);
  return pixmap;
}

TEST(FilterGraphExecutorTest, AppliesOffsetsUsingCapturedFilterTransform) {
  auto maybePixmap = tiny_skia::Pixmap::fromSize(8, 8);
  ASSERT_TRUE(maybePixmap.has_value());
  tiny_skia::Pixmap pixmap = std::move(*maybePixmap);

  SetPixel(pixmap, 1, 1, Pixel{255, 0, 0, 255});

  components::FilterGraph graph;
  components::FilterNode node;
  node.inputs.push_back(components::FilterInput{});
  node.primitive = components::filter_primitive::Offset{.dx = 1.0, .dy = 0.0};
  graph.nodes.push_back(std::move(node));

  ApplyFilterGraphToPixmap(pixmap, graph, Transformd::Scale(2.0), std::nullopt);

  EXPECT_EQ(GetPixel(pixmap, 1, 1), Pixel({0, 0, 0, 0}));
  EXPECT_EQ(GetPixel(pixmap, 3, 1), Pixel({255, 0, 0, 255}));
}

TEST(FilterGraphExecutorTest, RoundsFractionalOffsetsToNearestPixel) {
  auto maybePixmap = tiny_skia::Pixmap::fromSize(24, 24);
  ASSERT_TRUE(maybePixmap.has_value());
  tiny_skia::Pixmap pixmap = std::move(*maybePixmap);

  SetPixel(pixmap, 1, 1, Pixel{255, 0, 0, 255});

  components::FilterGraph graph;
  components::FilterNode node;
  node.inputs.push_back(components::FilterInput{});
  node.primitive = components::filter_primitive::Offset{.dx = 5.0, .dy = 0.0};
  graph.nodes.push_back(std::move(node));

  ApplyFilterGraphToPixmap(pixmap, graph, Transformd::Scale(2.5), std::nullopt);

  // dx=5.0 with Scale(2.5) → pixel offset = lround(12.5) = 13.
  // Pixel at (1,1) moves to (14,1).
  EXPECT_EQ(GetPixel(pixmap, 14, 1), Pixel({255, 0, 0, 255}));
  EXPECT_EQ(GetPixel(pixmap, 13, 1), Pixel({0, 0, 0, 0}));
}

TEST(FilterGraphExecutorTest, ClipsFilterOutputUsingCapturedFilterTransform) {
  auto maybePixmap = tiny_skia::Pixmap::fromSize(8, 8);
  ASSERT_TRUE(maybePixmap.has_value());
  tiny_skia::Pixmap pixmap = std::move(*maybePixmap);

  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      SetPixel(pixmap, x, y, Pixel{255, 255, 255, 255});
    }
  }

  ClipFilterOutputToRegion(pixmap, Boxd::FromXYWH(1.0, 1.0, 2.0, 2.0), Transformd::Scale(2.0));

  EXPECT_EQ(GetPixel(pixmap, 1, 1), Pixel({0, 0, 0, 0}));
  EXPECT_EQ(GetPixel(pixmap, 2, 2), Pixel({255, 255, 255, 255}));
  EXPECT_EQ(GetPixel(pixmap, 5, 5), Pixel({255, 255, 255, 255}));
  EXPECT_EQ(GetPixel(pixmap, 6, 6), Pixel({0, 0, 0, 0}));
}

TEST(FilterGraphExecutorTest, ClipsCompletelyOffscreenFilterRegion) {
  auto maybePixmap = tiny_skia::Pixmap::fromSize(8, 8);
  ASSERT_TRUE(maybePixmap.has_value());
  tiny_skia::Pixmap pixmap = std::move(*maybePixmap);

  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      SetPixel(pixmap, x, y, Pixel{255, 255, 255, 255});
    }
  }

  ClipFilterOutputToRegion(pixmap, Boxd::FromXYWH(-10.0, -10.0, 2.0, 2.0), Transformd());

  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      EXPECT_EQ(GetPixel(pixmap, x, y), Pixel({0, 0, 0, 0}));
    }
  }
}

TEST(FilterGraphExecutorTest, RotationDoesNotChangeBlurSigma) {
  const tiny_skia::Pixmap identityBlurred = CreateBlurredDotPixmap(Transformd());
  const tiny_skia::Pixmap rotatedBlurred = CreateBlurredDotPixmap(Transformd::Rotate(M_PI / 4.0));

  EXPECT_EQ(identityBlurred.data().size(), rotatedBlurred.data().size());
  for (std::size_t i = 0; i < identityBlurred.data().size(); ++i) {
    EXPECT_EQ(identityBlurred.data()[i], rotatedBlurred.data()[i]) << "byte index=" << i;
  }
}

TEST(FilterGraphExecutorTest, PrimitiveSubregionPercentagesUseUserSpaceAndFilterRegionClip) {
  auto maybePixmap = tiny_skia::Pixmap::fromSize(32, 32);
  ASSERT_TRUE(maybePixmap.has_value());
  tiny_skia::Pixmap pixmap = std::move(*maybePixmap);

  components::FilterGraph graph;
  components::FilterNode node;
  node.inputs.push_back(components::FilterInput{});
  node.primitive = components::filter_primitive::Flood{
      .floodColor = css::Color(css::RGBA(255, 0, 0, 255)),
      .floodOpacity = 1.0,
  };
  node.x = Lengthd(25.0, Lengthd::Unit::Percent);
  node.y = Lengthd(25.0, Lengthd::Unit::Percent);
  node.width = Lengthd(50.0, Lengthd::Unit::Percent);
  node.height = Lengthd(50.0, Lengthd::Unit::Percent);
  graph.nodes.push_back(std::move(node));

  ApplyFilterGraphToPixmap(pixmap, graph, Transformd(), Boxd::FromXYWH(4.0, 4.0, 16.0, 16.0));

  EXPECT_EQ(GetPixel(pixmap, 7, 7), Pixel({0, 0, 0, 0}));
  EXPECT_EQ(GetPixel(pixmap, 8, 8), Pixel({255, 0, 0, 255}));
  EXPECT_EQ(GetPixel(pixmap, 19, 19), Pixel({255, 0, 0, 255}));
  EXPECT_EQ(GetPixel(pixmap, 20, 20), Pixel({0, 0, 0, 0}));
}

TEST(FilterGraphExecutorTest, PrimitiveSubregionPercentagesUseElementBoundsInObjectBoundingBox) {
  auto maybePixmap = tiny_skia::Pixmap::fromSize(32, 32);
  ASSERT_TRUE(maybePixmap.has_value());
  tiny_skia::Pixmap pixmap = std::move(*maybePixmap);

  components::FilterGraph graph;
  graph.primitiveUnits = PrimitiveUnits::ObjectBoundingBox;
  graph.elementBoundingBox = Boxd::FromXYWH(10.0, 10.0, 20.0, 20.0);

  components::FilterNode node;
  node.inputs.push_back(components::FilterInput{});
  node.primitive = components::filter_primitive::Flood{
      .floodColor = css::Color(css::RGBA(0, 255, 0, 255)),
      .floodOpacity = 1.0,
  };
  node.width = Lengthd(50.0, Lengthd::Unit::Percent);
  node.height = Lengthd(50.0, Lengthd::Unit::Percent);
  graph.nodes.push_back(std::move(node));

  ApplyFilterGraphToPixmap(pixmap, graph, Transformd(), std::nullopt);

  EXPECT_EQ(GetPixel(pixmap, 9, 9), Pixel({0, 0, 0, 0}));
  EXPECT_EQ(GetPixel(pixmap, 10, 10), Pixel({0, 255, 0, 255}));
  EXPECT_EQ(GetPixel(pixmap, 19, 19), Pixel({0, 255, 0, 255}));
  EXPECT_EQ(GetPixel(pixmap, 20, 20), Pixel({0, 0, 0, 0}));
}

TEST(FilterGraphExecutorTest, GaussianBlurExpandsDefaultPrimitiveSubregion) {
  auto maybePixmap = tiny_skia::Pixmap::fromSize(32, 32);
  ASSERT_TRUE(maybePixmap.has_value());
  tiny_skia::Pixmap pixmap = std::move(*maybePixmap);

  components::FilterGraph graph;

  components::FilterNode floodNode;
  floodNode.inputs.push_back(components::FilterInput{});
  floodNode.primitive = components::filter_primitive::Flood{
      .floodColor = css::Color(css::RGBA(255, 255, 255, 255)),
      .floodOpacity = 1.0,
  };
  floodNode.x = Lengthd(16.0);
  floodNode.y = Lengthd(16.0);
  floodNode.width = Lengthd(1.0);
  floodNode.height = Lengthd(1.0);
  graph.nodes.push_back(std::move(floodNode));

  components::FilterNode blurNode;
  blurNode.inputs.push_back(components::FilterInput{});
  blurNode.primitive = components::filter_primitive::GaussianBlur{
      .stdDeviationX = 2.0,
      .stdDeviationY = 2.0,
  };
  graph.nodes.push_back(std::move(blurNode));

  ApplyFilterGraphToPixmap(pixmap, graph, Transformd(), Boxd::FromXYWH(0.0, 0.0, 32.0, 32.0));

  // Flood at (16,16) size 1x1, blur σ=2 → subregion expands by ceil(2*3)=6.
  // Expanded subregion: x=[10,23], y=[10,23]. Pixel (9,16) is outside.
  EXPECT_EQ(GetPixel(pixmap, 9, 16), Pixel({0, 0, 0, 0}));
  EXPECT_GT(GetPixel(pixmap, 15, 16)[3], 0);
  EXPECT_GT(GetPixel(pixmap, 16, 15)[3], 0);
}

TEST(FilterGraphExecutorTest, FilterRegionClipsInitialSourceGraphic) {
  auto maybePixmap = tiny_skia::Pixmap::fromSize(16, 8);
  ASSERT_TRUE(maybePixmap.has_value());
  tiny_skia::Pixmap pixmap = std::move(*maybePixmap);

  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 16; ++x) {
      SetPixel(pixmap, x, y, Pixel{255, 255, 255, 255});
    }
  }

  components::FilterGraph graph;
  components::FilterNode node;
  node.inputs.push_back(components::FilterInput{});
  node.primitive = components::filter_primitive::GaussianBlur{
      .stdDeviationX = 1.0,
      .stdDeviationY = 1.0,
  };
  graph.nodes.push_back(std::move(node));

  ApplyFilterGraphToPixmap(pixmap, graph, Transformd(), Boxd::FromXYWH(0.0, 0.0, 8.0, 8.0), true);

  EXPECT_GT(GetPixel(pixmap, 7, 4)[3], 0);
  EXPECT_LT(GetPixel(pixmap, 7, 4)[3], 255);
  EXPECT_EQ(GetPixel(pixmap, 12, 4), Pixel({0, 0, 0, 0}));
}

TEST(FilterGraphExecutorTest, UsesFillPaintInputWhenRequested) {
  auto maybePixmap = tiny_skia::Pixmap::fromSize(8, 8);
  auto maybeFillPaintPixmap = tiny_skia::Pixmap::fromSize(8, 8);
  ASSERT_TRUE(maybePixmap.has_value());
  ASSERT_TRUE(maybeFillPaintPixmap.has_value());
  tiny_skia::Pixmap pixmap = std::move(*maybePixmap);
  tiny_skia::Pixmap fillPaintPixmap = std::move(*maybeFillPaintPixmap);

  SetPixel(pixmap, 1, 1, Pixel{255, 0, 0, 255});
  SetPixel(fillPaintPixmap, 2, 2, Pixel{0, 255, 0, 255});

  components::FilterGraph graph;
  components::FilterNode node;
  node.inputs.push_back(components::FilterInput{components::FilterStandardInput::FillPaint});
  node.primitive = components::filter_primitive::Offset{.dx = 0.0, .dy = 0.0};
  graph.nodes.push_back(std::move(node));

  ApplyFilterGraphToPixmap(pixmap, graph, Transformd(), std::nullopt, false, &fillPaintPixmap);

  EXPECT_EQ(GetPixel(pixmap, 1, 1), Pixel({0, 0, 0, 0}));
  EXPECT_EQ(GetPixel(pixmap, 2, 2), Pixel({0, 255, 0, 255}));
}

TEST(FilterGraphExecutorTest, MissingStrokePaintInputDefaultsToTransparent) {
  auto maybePixmap = tiny_skia::Pixmap::fromSize(8, 8);
  ASSERT_TRUE(maybePixmap.has_value());
  tiny_skia::Pixmap pixmap = std::move(*maybePixmap);

  SetPixel(pixmap, 1, 1, Pixel{255, 0, 0, 255});

  components::FilterGraph graph;
  components::FilterNode node;
  node.inputs.push_back(components::FilterInput{components::FilterStandardInput::StrokePaint});
  node.primitive = components::filter_primitive::Offset{.dx = 0.0, .dy = 0.0};
  graph.nodes.push_back(std::move(node));

  ApplyFilterGraphToPixmap(pixmap, graph, Transformd(), std::nullopt);

  EXPECT_EQ(GetPixel(pixmap, 1, 1), Pixel({0, 0, 0, 0}));
}

// ---------------------------------------------------------------------------
// Multi-node filter chains
// ---------------------------------------------------------------------------

TEST(FilterGraphExecutorTest, ThreeNodeChainBlurOffsetFlood) {
  // Flood -> Offset -> Blur chain using implicit previous-result wiring.
  auto maybePixmap = tiny_skia::Pixmap::fromSize(32, 32);
  ASSERT_TRUE(maybePixmap.has_value());
  tiny_skia::Pixmap pixmap = std::move(*maybePixmap);

  components::FilterGraph graph;

  // Node 0: Flood red into the entire region.
  components::FilterNode floodNode;
  floodNode.inputs.push_back(components::FilterInput{});
  floodNode.primitive = components::filter_primitive::Flood{
      .floodColor = css::Color(css::RGBA(255, 0, 0, 255)),
      .floodOpacity = 1.0,
  };
  graph.nodes.push_back(std::move(floodNode));

  // Node 1: Offset the flood result by (4,0).
  components::FilterNode offsetNode;
  offsetNode.inputs.push_back(components::FilterInput{});  // previous
  offsetNode.primitive = components::filter_primitive::Offset{.dx = 4.0, .dy = 0.0};
  graph.nodes.push_back(std::move(offsetNode));

  // Node 2: Blur the offset result with small sigma.
  components::FilterNode blurNode;
  blurNode.inputs.push_back(components::FilterInput{});  // previous
  blurNode.primitive = components::filter_primitive::GaussianBlur{
      .stdDeviationX = 1.0,
      .stdDeviationY = 1.0,
  };
  graph.nodes.push_back(std::move(blurNode));

  ApplyFilterGraphToPixmap(pixmap, graph, Transformd(), Boxd::FromXYWH(0.0, 0.0, 32.0, 32.0));

  // After offset by 4, columns 0-3 should be transparent. Column 0 should remain zero even after
  // the small blur because sigma=1 doesn't spread 4 pixels.
  EXPECT_EQ(GetPixel(pixmap, 0, 16), Pixel({0, 0, 0, 0}));
  // Deep inside the offset region, the pixel should still be red (possibly slightly blurred at
  // edges, but center stays opaque).
  const Pixel center = GetPixel(pixmap, 20, 16);
  EXPECT_EQ(center[0], 255);
  EXPECT_EQ(center[3], 255);
}

TEST(FilterGraphExecutorTest, FourNodeChainFloodOffsetBlurMerge) {
  // Flood -> Offset -> Blur, then Merge(SourceGraphic, blurred).
  auto maybePixmap = tiny_skia::Pixmap::fromSize(16, 16);
  ASSERT_TRUE(maybePixmap.has_value());
  tiny_skia::Pixmap pixmap = std::move(*maybePixmap);

  // Set a green pixel in source graphic.
  SetPixel(pixmap, 8, 8, Pixel{0, 255, 0, 255});

  components::FilterGraph graph;

  // Node 0: Flood blue.
  components::FilterNode floodNode;
  floodNode.inputs.push_back(components::FilterInput{});
  floodNode.primitive = components::filter_primitive::Flood{
      .floodColor = css::Color(css::RGBA(0, 0, 255, 255)),
      .floodOpacity = 1.0,
  };
  floodNode.x = Lengthd(4.0);
  floodNode.y = Lengthd(4.0);
  floodNode.width = Lengthd(4.0);
  floodNode.height = Lengthd(4.0);
  graph.nodes.push_back(std::move(floodNode));

  // Node 1: Offset the flood by (2, 2).
  components::FilterNode offsetNode;
  offsetNode.inputs.push_back(components::FilterInput{});  // previous
  offsetNode.primitive = components::filter_primitive::Offset{.dx = 2.0, .dy = 2.0};
  graph.nodes.push_back(std::move(offsetNode));

  // Node 2: Merge SourceGraphic with the offset flood.
  components::FilterNode mergeNode;
  mergeNode.inputs.push_back(
      components::FilterInput{components::FilterStandardInput::SourceGraphic});
  mergeNode.inputs.push_back(components::FilterInput{});  // previous (offset result)
  mergeNode.primitive = components::filter_primitive::Merge{};
  graph.nodes.push_back(std::move(mergeNode));

  ApplyFilterGraphToPixmap(pixmap, graph, Transformd(), Boxd::FromXYWH(0.0, 0.0, 16.0, 16.0));

  // The green source pixel at (8,8) should still be present.
  const Pixel greenPixel = GetPixel(pixmap, 8, 8);
  EXPECT_GT(greenPixel[1], 0);
  EXPECT_EQ(greenPixel[3], 255);

  // The flood was at (4,4)-(8,8) and was offset by (2,2), so the blue region is at (6,6)-(10,10).
  // Check a pixel inside the flood region that isn't (8,8).
  const Pixel bluePixel = GetPixel(pixmap, 7, 7);
  EXPECT_GT(bluePixel[2], 0);
  EXPECT_EQ(bluePixel[3], 255);
}

// ---------------------------------------------------------------------------
// Named buffer routing
// ---------------------------------------------------------------------------

TEST(FilterGraphExecutorTest, NamedResultRoutesCorrectBuffer) {
  auto maybePixmap = tiny_skia::Pixmap::fromSize(8, 8);
  ASSERT_TRUE(maybePixmap.has_value());
  tiny_skia::Pixmap pixmap = std::move(*maybePixmap);

  // Fill SourceGraphic with white.
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      SetPixel(pixmap, x, y, Pixel{255, 255, 255, 255});
    }
  }

  components::FilterGraph graph;

  // Node 0: Flood red, store as result="redFlood".
  components::FilterNode floodNode;
  floodNode.inputs.push_back(components::FilterInput{});
  floodNode.primitive = components::filter_primitive::Flood{
      .floodColor = css::Color(css::RGBA(255, 0, 0, 255)),
      .floodOpacity = 1.0,
  };
  floodNode.result = RcString("redFlood");
  graph.nodes.push_back(std::move(floodNode));

  // Node 1: Flood green (overwrites the implicit previous result but doesn't touch "redFlood").
  components::FilterNode greenFloodNode;
  greenFloodNode.inputs.push_back(components::FilterInput{});
  greenFloodNode.primitive = components::filter_primitive::Flood{
      .floodColor = css::Color(css::RGBA(0, 255, 0, 255)),
      .floodOpacity = 1.0,
  };
  graph.nodes.push_back(std::move(greenFloodNode));

  // Node 2: Identity offset referencing "redFlood" by name — should produce red, not green.
  components::FilterNode offsetNode;
  offsetNode.inputs.push_back(
      components::FilterInput{components::FilterInput::Named{RcString("redFlood")}});
  offsetNode.primitive = components::filter_primitive::Offset{.dx = 0.0, .dy = 0.0};
  graph.nodes.push_back(std::move(offsetNode));

  ApplyFilterGraphToPixmap(pixmap, graph, Transformd(), std::nullopt);

  // The final output should be the red flood (from the named buffer), not the green one.
  EXPECT_EQ(GetPixel(pixmap, 4, 4), Pixel({255, 0, 0, 255}));
}

TEST(FilterGraphExecutorTest, NamedResultCanBeReusedMultipleTimes) {
  auto maybePixmap = tiny_skia::Pixmap::fromSize(16, 16);
  ASSERT_TRUE(maybePixmap.has_value());
  tiny_skia::Pixmap pixmap = std::move(*maybePixmap);

  components::FilterGraph graph;

  // Node 0: Flood blue, store as result="blueFlood".
  components::FilterNode floodNode;
  floodNode.inputs.push_back(components::FilterInput{});
  floodNode.primitive = components::filter_primitive::Flood{
      .floodColor = css::Color(css::RGBA(0, 0, 255, 255)),
      .floodOpacity = 1.0,
  };
  floodNode.result = RcString("blueFlood");
  graph.nodes.push_back(std::move(floodNode));

  // Node 1: Merge the named buffer with itself — both inputs reference "blueFlood".
  components::FilterNode mergeNode;
  mergeNode.inputs.push_back(
      components::FilterInput{components::FilterInput::Named{RcString("blueFlood")}});
  mergeNode.inputs.push_back(
      components::FilterInput{components::FilterInput::Named{RcString("blueFlood")}});
  mergeNode.primitive = components::filter_primitive::Merge{};
  graph.nodes.push_back(std::move(mergeNode));

  ApplyFilterGraphToPixmap(pixmap, graph, Transformd(), std::nullopt);

  // Output should be blue (merged blue over blue = blue).
  EXPECT_EQ(GetPixel(pixmap, 8, 8), Pixel({0, 0, 255, 255}));
}

// ---------------------------------------------------------------------------
// CSS shorthand-style filter (drop-shadow)
// ---------------------------------------------------------------------------

TEST(FilterGraphExecutorTest, DropShadowProducesOffsetAndBlurredCopy) {
  auto maybePixmap = tiny_skia::Pixmap::fromSize(32, 32);
  ASSERT_TRUE(maybePixmap.has_value());
  tiny_skia::Pixmap pixmap = std::move(*maybePixmap);

  // Place a white dot in the center.
  SetPixel(pixmap, 16, 16, Pixel{255, 255, 255, 255});

  components::FilterGraph graph;
  components::FilterNode node;
  node.inputs.push_back(components::FilterInput{});
  node.primitive = components::filter_primitive::DropShadow{
      .dx = 4.0,
      .dy = 4.0,
      .stdDeviationX = 1.0,
      .stdDeviationY = 1.0,
      .floodColor = css::Color(css::RGBA(0, 0, 0, 255)),
      .floodOpacity = 1.0,
  };
  graph.nodes.push_back(std::move(node));

  ApplyFilterGraphToPixmap(pixmap, graph, Transformd(), Boxd::FromXYWH(0.0, 0.0, 32.0, 32.0));

  // The original white dot should still be present.
  const Pixel orig = GetPixel(pixmap, 16, 16);
  EXPECT_EQ(orig[0], 255);
  EXPECT_EQ(orig[3], 255);

  // The shadow center should be near (20, 20), with non-zero alpha (black shadow).
  const Pixel shadow = GetPixel(pixmap, 20, 20);
  EXPECT_GT(shadow[3], 0);
}

// ---------------------------------------------------------------------------
// Error / edge cases
// ---------------------------------------------------------------------------

TEST(FilterGraphExecutorTest, EmptyFilterGraphLeavesPixmapUnchanged) {
  auto maybePixmap = tiny_skia::Pixmap::fromSize(8, 8);
  ASSERT_TRUE(maybePixmap.has_value());
  tiny_skia::Pixmap pixmap = std::move(*maybePixmap);

  SetPixel(pixmap, 3, 3, Pixel{42, 100, 200, 255});

  components::FilterGraph graph;
  // graph.nodes is empty.

  ApplyFilterGraphToPixmap(pixmap, graph, Transformd(), std::nullopt);

  // The pixel should remain unchanged.
  EXPECT_EQ(GetPixel(pixmap, 3, 3), Pixel({42, 100, 200, 255}));
}

TEST(FilterGraphExecutorTest, FilterWithNoSourceGraphicContent) {
  // SourceGraphic is entirely transparent — filter should still execute.
  auto maybePixmap = tiny_skia::Pixmap::fromSize(8, 8);
  ASSERT_TRUE(maybePixmap.has_value());
  tiny_skia::Pixmap pixmap = std::move(*maybePixmap);
  // Pixmap starts as all-zero (transparent).

  components::FilterGraph graph;
  components::FilterNode node;
  node.inputs.push_back(components::FilterInput{});
  node.primitive = components::filter_primitive::Flood{
      .floodColor = css::Color(css::RGBA(128, 64, 32, 255)),
      .floodOpacity = 1.0,
  };
  graph.nodes.push_back(std::move(node));

  ApplyFilterGraphToPixmap(pixmap, graph, Transformd(), std::nullopt);

  // Even though SourceGraphic was transparent, the flood should fill the pixmap.
  EXPECT_EQ(GetPixel(pixmap, 4, 4), Pixel({128, 64, 32, 255}));
}

TEST(FilterGraphExecutorTest, SourceAlphaInputExtractsAlphaChannel) {
  auto maybePixmap = tiny_skia::Pixmap::fromSize(8, 8);
  ASSERT_TRUE(maybePixmap.has_value());
  tiny_skia::Pixmap pixmap = std::move(*maybePixmap);

  // Set a colored pixel with full alpha.
  SetPixel(pixmap, 4, 4, Pixel{200, 100, 50, 255});

  components::FilterGraph graph;
  components::FilterNode node;
  // Use SourceAlpha as input — should zero out RGB, keep alpha.
  node.inputs.push_back(components::FilterInput{components::FilterStandardInput::SourceAlpha});
  node.primitive = components::filter_primitive::Offset{.dx = 0.0, .dy = 0.0};
  graph.nodes.push_back(std::move(node));

  ApplyFilterGraphToPixmap(pixmap, graph, Transformd(), std::nullopt);

  const Pixel result = GetPixel(pixmap, 4, 4);
  // SourceAlpha produces (0, 0, 0, alpha).
  EXPECT_EQ(result[0], 0);
  EXPECT_EQ(result[1], 0);
  EXPECT_EQ(result[2], 0);
  EXPECT_EQ(result[3], 255);
}

// ---------------------------------------------------------------------------
// Color space handling
// ---------------------------------------------------------------------------

TEST(FilterGraphExecutorTest, SRGBColorInterpolationProducesDifferentResultThanLinearRGB) {
  // Blend two colors using sRGB vs linearRGB and verify the results differ.
  auto createBlendedPixmap = [](ColorInterpolationFilters colorSpace) -> tiny_skia::Pixmap {
    auto maybePixmap = tiny_skia::Pixmap::fromSize(8, 8);
    tiny_skia::Pixmap pixmap = std::move(*maybePixmap);

    // Fill with a mid-tone red.
    for (int y = 0; y < 8; ++y) {
      for (int x = 0; x < 8; ++x) {
        SetPixel(pixmap, x, y, Pixel{128, 0, 0, 255});
      }
    }

    components::FilterGraph graph;
    graph.colorInterpolationFilters = colorSpace;

    // Flood green over the red source using a blend.
    components::FilterNode floodNode;
    floodNode.inputs.push_back(components::FilterInput{});
    floodNode.primitive = components::filter_primitive::Flood{
        .floodColor = css::Color(css::RGBA(0, 128, 0, 128)),
        .floodOpacity = 1.0,
    };
    floodNode.result = RcString("green");
    graph.nodes.push_back(std::move(floodNode));

    components::FilterNode blendNode;
    blendNode.inputs.push_back(
        components::FilterInput{components::FilterStandardInput::SourceGraphic});
    blendNode.inputs.push_back(
        components::FilterInput{components::FilterInput::Named{RcString("green")}});
    blendNode.primitive = components::filter_primitive::Blend{
        .mode = components::filter_primitive::Blend::Mode::Normal,
    };
    graph.nodes.push_back(std::move(blendNode));

    ApplyFilterGraphToPixmap(pixmap, graph, Transformd(), std::nullopt);
    return pixmap;
  };

  const tiny_skia::Pixmap srgbResult = createBlendedPixmap(ColorInterpolationFilters::SRGB);
  const tiny_skia::Pixmap linearResult = createBlendedPixmap(ColorInterpolationFilters::LinearRGB);

  // At least some pixel values should differ between the two color spaces.
  bool anyDifference = false;
  for (std::size_t i = 0; i < srgbResult.data().size(); ++i) {
    if (srgbResult.data()[i] != linearResult.data()[i]) {
      anyDifference = true;
      break;
    }
  }
  EXPECT_TRUE(anyDifference) << "sRGB and linearRGB blending should produce different results";
}

TEST(FilterGraphExecutorTest, PerNodeColorInterpolationOverridesGraphDefault) {
  // Graph defaults to linearRGB, but one node overrides to sRGB.
  auto maybePixmap = tiny_skia::Pixmap::fromSize(8, 8);
  ASSERT_TRUE(maybePixmap.has_value());
  tiny_skia::Pixmap pixmap = std::move(*maybePixmap);

  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      SetPixel(pixmap, x, y, Pixel{128, 128, 128, 255});
    }
  }

  components::FilterGraph graph;
  graph.colorInterpolationFilters = ColorInterpolationFilters::LinearRGB;

  // A blur node that overrides to sRGB.
  components::FilterNode blurNode;
  blurNode.inputs.push_back(components::FilterInput{});
  blurNode.primitive = components::filter_primitive::GaussianBlur{
      .stdDeviationX = 1.0,
      .stdDeviationY = 1.0,
  };
  blurNode.colorInterpolationFilters = ColorInterpolationFilters::SRGB;
  graph.nodes.push_back(std::move(blurNode));

  // This should execute without crashing. For a uniform pixmap, blur is a no-op, but the
  // color-space conversion path is exercised.
  ApplyFilterGraphToPixmap(pixmap, graph, Transformd(), std::nullopt);

  // Center pixel should still be mid-gray (blur of uniform input is identity).
  const Pixel center = GetPixel(pixmap, 4, 4);
  EXPECT_NEAR(center[0], 128, 2);
  EXPECT_NEAR(center[1], 128, 2);
  EXPECT_NEAR(center[2], 128, 2);
  EXPECT_EQ(center[3], 255);
}

// ---------------------------------------------------------------------------
// Composite operator tests
// ---------------------------------------------------------------------------

TEST(FilterGraphExecutorTest, CompositeInOperatorKeepsOverlapOnly) {
  auto maybePixmap = tiny_skia::Pixmap::fromSize(8, 8);
  ASSERT_TRUE(maybePixmap.has_value());
  tiny_skia::Pixmap pixmap = std::move(*maybePixmap);

  // Source graphic: red pixel at (2,2).
  SetPixel(pixmap, 2, 2, Pixel{255, 0, 0, 255});
  // Also at (4,4) for overlap test.
  SetPixel(pixmap, 4, 4, Pixel{255, 0, 0, 255});

  components::FilterGraph graph;

  // Node 0: Flood green in a subregion that includes (4,4) but not (2,2).
  components::FilterNode floodNode;
  floodNode.inputs.push_back(components::FilterInput{});
  floodNode.primitive = components::filter_primitive::Flood{
      .floodColor = css::Color(css::RGBA(0, 255, 0, 255)),
      .floodOpacity = 1.0,
  };
  floodNode.x = Lengthd(3.0);
  floodNode.y = Lengthd(3.0);
  floodNode.width = Lengthd(4.0);
  floodNode.height = Lengthd(4.0);
  floodNode.result = RcString("greenRegion");
  graph.nodes.push_back(std::move(floodNode));

  // Node 1: Composite SourceGraphic IN greenRegion — only keep source pixels where green is opaque.
  components::FilterNode compositeNode;
  compositeNode.inputs.push_back(
      components::FilterInput{components::FilterStandardInput::SourceGraphic});
  compositeNode.inputs.push_back(
      components::FilterInput{components::FilterInput::Named{RcString("greenRegion")}});
  compositeNode.primitive = components::filter_primitive::Composite{
      .op = components::filter_primitive::Composite::Operator::In,
  };
  graph.nodes.push_back(std::move(compositeNode));

  ApplyFilterGraphToPixmap(pixmap, graph, Transformd(), Boxd::FromXYWH(0.0, 0.0, 8.0, 8.0));

  // (2,2) is outside the green region → should be transparent.
  EXPECT_EQ(GetPixel(pixmap, 2, 2), Pixel({0, 0, 0, 0}));
  // (4,4) is inside the green region → should keep the red pixel.
  EXPECT_EQ(GetPixel(pixmap, 4, 4), Pixel({255, 0, 0, 255}));
}

// ---------------------------------------------------------------------------
// Blend mode (multiply)
// ---------------------------------------------------------------------------

TEST(FilterGraphExecutorTest, BlendMultiplyDarkensColors) {
  auto maybePixmap = tiny_skia::Pixmap::fromSize(8, 8);
  ASSERT_TRUE(maybePixmap.has_value());
  tiny_skia::Pixmap pixmap = std::move(*maybePixmap);

  // Fill with white.
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      SetPixel(pixmap, x, y, Pixel{255, 255, 255, 255});
    }
  }

  components::FilterGraph graph;
  // Use sRGB to get straightforward multiply behavior.
  graph.colorInterpolationFilters = ColorInterpolationFilters::SRGB;

  // Flood 50% gray.
  components::FilterNode floodNode;
  floodNode.inputs.push_back(components::FilterInput{});
  floodNode.primitive = components::filter_primitive::Flood{
      .floodColor = css::Color(css::RGBA(128, 128, 128, 255)),
      .floodOpacity = 1.0,
  };
  floodNode.result = RcString("gray");
  graph.nodes.push_back(std::move(floodNode));

  // Blend SourceGraphic (white) * gray → should produce ~128.
  components::FilterNode blendNode;
  blendNode.inputs.push_back(
      components::FilterInput{components::FilterStandardInput::SourceGraphic});
  blendNode.inputs.push_back(
      components::FilterInput{components::FilterInput::Named{RcString("gray")}});
  blendNode.primitive = components::filter_primitive::Blend{
      .mode = components::filter_primitive::Blend::Mode::Multiply,
  };
  graph.nodes.push_back(std::move(blendNode));

  ApplyFilterGraphToPixmap(pixmap, graph, Transformd(), std::nullopt);

  const Pixel result = GetPixel(pixmap, 4, 4);
  // Multiply: white * 128/255 ≈ 128. Allow tolerance for rounding.
  EXPECT_NEAR(result[0], 128, 2);
  EXPECT_NEAR(result[1], 128, 2);
  EXPECT_NEAR(result[2], 128, 2);
  EXPECT_EQ(result[3], 255);
}

}  // namespace
}  // namespace donner::svg
