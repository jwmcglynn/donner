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

  EXPECT_EQ(GetPixel(pixmap, 13, 1), Pixel({255, 0, 0, 255}));
  EXPECT_EQ(GetPixel(pixmap, 12, 1), Pixel({0, 0, 0, 0}));
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

  EXPECT_EQ(GetPixel(pixmap, 12, 16), Pixel({0, 0, 0, 0}));
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

}  // namespace
}  // namespace donner::svg
