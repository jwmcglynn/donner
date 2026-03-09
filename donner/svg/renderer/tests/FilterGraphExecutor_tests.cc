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

TEST(FilterGraphExecutorTest, RotationDoesNotChangeBlurSigma) {
  const tiny_skia::Pixmap identityBlurred = CreateBlurredDotPixmap(Transformd());
  const tiny_skia::Pixmap rotatedBlurred = CreateBlurredDotPixmap(Transformd::Rotate(M_PI / 4.0));

  EXPECT_EQ(identityBlurred.data().size(), rotatedBlurred.data().size());
  for (std::size_t i = 0; i < identityBlurred.data().size(); ++i) {
    EXPECT_EQ(identityBlurred.data()[i], rotatedBlurred.data()[i]) << "byte index=" << i;
  }
}

}  // namespace
}  // namespace donner::svg
