/// Tests for the color-space boundaries `executeFilterGraph` places around a filter graph.
///
/// The executor tags every buffer with the space its pixels are in and converts only where two
/// adjacent primitives disagree, instead of converting into and out of linearRGB around each
/// primitive. These tests pin the resulting conversion placement against hand-written references
/// built from the same primitives and the same `ColorSpace` entry points:
///   - a graph whose primitives all share one space converts exactly twice, on entry and exit;
///   - a graph that alternates spaces converts on each crossing, which is bit-for-bit what
///     converting around every primitive used to produce;
///   - a graph that is entirely sRGB converts not at all.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "tiny_skia/Pixmap.h"
#include "tiny_skia/filter/ColorMatrix.h"
#include "tiny_skia/filter/ColorSpace.h"
#include "tiny_skia/filter/FilterGraph.h"
#include "tiny_skia/filter/FloatPixmap.h"
#include "tiny_skia/filter/Flood.h"
#include "tiny_skia/filter/GaussianBlur.h"
#include "tiny_skia/filter/Merge.h"

namespace tiny_skia::filter {
namespace {

constexpr int kWidth = 24;
constexpr int kHeight = 24;
constexpr double kSigma = 1.5;

/// A premultiplied sRGB flood color with partial alpha, far enough from both ends of the transfer
/// function that a missing or duplicated conversion changes the bytes.
constexpr std::uint8_t kFloodR = 200;
constexpr std::uint8_t kFloodG = 80;
constexpr std::uint8_t kFloodB = 30;
constexpr std::uint8_t kFloodA = 190;

/// A saturating color matrix, chosen because it is far from the identity (which the executor
/// short-circuits) and mixes the channels, so a misplaced conversion shows up in the result.
std::array<double, 20> saturateMatrix() {
  return {0.8, 0.3, -0.1, 0.0, 0.0,  //
          0.1, 0.9, 0.0,  0.0, 0.0,  //
          0.0, 0.2, 0.7,  0.0, 0.0,  //
          0.0, 0.0, 0.0,  1.0, 0.0};
}

/// Builds a source graphic with saturated colors, mid-tones, and partial alpha, so that both the
/// toe and the power segment of the transfer function are exercised, and premultiplied values are
/// not all equal to their unpremultiplied form.
Pixmap makeSourceGraphic() {
  auto maybePixmap = Pixmap::fromSize(kWidth, kHeight);
  EXPECT_TRUE(maybePixmap.has_value());
  Pixmap pixmap = std::move(*maybePixmap);

  auto data = pixmap.data();
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      const auto alpha = static_cast<std::uint8_t>(40 + (x * 9 + y * 3) % 216);
      const auto scale = [&](int value) { return static_cast<std::uint8_t>(value * alpha / 255); };

      const std::size_t offset = static_cast<std::size_t>((y * kWidth + x) * 4);
      data[offset + 0] = scale((x * 11) % 256);
      data[offset + 1] = scale((y * 7 + 3) % 256);
      data[offset + 2] = scale((x * 5 + y * 13) % 256);
      data[offset + 3] = alpha;
    }
  }

  return pixmap;
}

/// Every node in these chains consumes the previous primitive's result, which for the first node
/// is SourceGraphic.
GraphNode chainedNode(GraphPrimitive primitive, bool linearRGB) {
  GraphNode node;
  node.primitive = std::move(primitive);
  node.inputs = {NodeInput()};
  node.useLinearRGB = linearRGB;
  return node;
}

GraphNode blurNode(bool linearRGB) {
  return chainedNode(graph_primitive::GaussianBlur{kSigma, kSigma, BlurEdgeMode::None}, linearRGB);
}

GraphNode colorMatrixNode(bool linearRGB) {
  return chainedNode(graph_primitive::ColorMatrix{saturateMatrix()}, linearRGB);
}

/// Runs blur, color matrix, blur over `source` with an explicit conversion schedule. Each entry of
/// `linearSteps` says whether the pixels are converted to linearRGB before that step and back to
/// sRGB after it; `false` runs the step on the sRGB values directly. This is the conversion
/// schedule the executor used to run unconditionally, once per primitive.
Pixmap runChainWithPerStepConversions(const Pixmap& source, std::array<bool, 3> linearSteps) {
  FloatPixmap fp = FloatPixmap::fromPixmap(source);

  const auto runStep = [&](int index, auto&& step) {
    if (linearSteps[index]) {
      srgbToLinear(fp);
    }
    step();
    if (linearSteps[index]) {
      linearToSrgb(fp);
    }
  };

  runStep(0, [&] { gaussianBlur(fp, kSigma, kSigma, BlurEdgeMode::None); });
  runStep(1, [&] { colorMatrix(fp, saturateMatrix()); });
  runStep(2, [&] { gaussianBlur(fp, kSigma, kSigma, BlurEdgeMode::None); });

  return fp.toPixmap();
}

/// Largest absolute per-channel difference between two same-sized pixmaps.
int maxChannelDelta(const Pixmap& lhs, const Pixmap& rhs) {
  const auto a = lhs.data();
  const auto b = rhs.data();
  EXPECT_EQ(a.size(), b.size());

  int worst = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const int delta = std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
    worst = std::max(worst, delta);
  }
  return worst;
}

::testing::AssertionResult PixmapsEqual(const Pixmap& actual, const Pixmap& expected) {
  const auto a = actual.data();
  const auto b = expected.data();
  if (a.size() != b.size()) {
    return ::testing::AssertionFailure() << "size mismatch: " << a.size() << " vs " << b.size();
  }

  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i] != b[i]) {
      return ::testing::AssertionFailure()
             << "first difference at byte " << i << " (pixel " << (i / 4) << ", channel " << (i % 4)
             << "): " << static_cast<int>(a[i]) << " vs " << static_cast<int>(b[i]) << ", "
             << maxChannelDelta(actual, expected) << " max channel delta overall";
    }
  }

  return ::testing::AssertionSuccess();
}

// A graph whose primitives all interpolate in linearRGB converts on the way in and on the way
// out, and nowhere in between: intermediate results stay linear.
TEST(FilterGraphColorSpace, UniformLinearGraphConvertsOnlyAtTheBoundaries) {
  const Pixmap source = makeSourceGraphic();

  FilterGraph graph;
  graph.useLinearRGB = true;
  graph.nodes = {blurNode(true), colorMatrixNode(true), blurNode(true)};

  Pixmap actual = source;
  ASSERT_TRUE(executeFilterGraph(actual, graph));

  FloatPixmap fp = FloatPixmap::fromPixmap(source);
  srgbToLinear(fp);
  gaussianBlur(fp, kSigma, kSigma, BlurEdgeMode::None);
  colorMatrix(fp, saturateMatrix());
  gaussianBlur(fp, kSigma, kSigma, BlurEdgeMode::None);
  linearToSrgb(fp);
  const Pixmap expected = fp.toPixmap();

  EXPECT_TRUE(PixmapsEqual(actual, expected));
}

// Converting around every primitive is the same computation with two extra round trips through
// the transfer function, so the two results have to agree to within rounding. This chain measures
// a worst-case channel difference of 1/255; the bound keeps a unit of headroom.
TEST(FilterGraphColorSpace, HoistingStaysWithinRoundingOfPerPrimitiveConversions) {
  const Pixmap source = makeSourceGraphic();

  FilterGraph graph;
  graph.useLinearRGB = true;
  graph.nodes = {blurNode(true), colorMatrixNode(true), blurNode(true)};

  Pixmap actual = source;
  ASSERT_TRUE(executeFilterGraph(actual, graph));

  const Pixmap perPrimitive = runChainWithPerStepConversions(source, {true, true, true});
  EXPECT_LE(maxChannelDelta(actual, perPrimitive), 2);
}

// An sRGB primitive between two linearRGB ones has to convert on both crossings, which is exactly
// the conversion schedule the per-primitive code ran. This case must not shift at all.
TEST(FilterGraphColorSpace, MixedGraphMatchesPerPrimitiveConversions) {
  const Pixmap source = makeSourceGraphic();

  FilterGraph graph;
  graph.useLinearRGB = true;
  graph.nodes = {blurNode(true), colorMatrixNode(false), blurNode(true)};

  Pixmap actual = source;
  ASSERT_TRUE(executeFilterGraph(actual, graph));

  const Pixmap expected = runChainWithPerStepConversions(source, {true, false, true});
  EXPECT_TRUE(PixmapsEqual(actual, expected));
}

// The mirror image: a linearRGB primitive between two sRGB ones.
TEST(FilterGraphColorSpace, MixedGraphWithLinearInteriorMatchesPerPrimitiveConversions) {
  const Pixmap source = makeSourceGraphic();

  FilterGraph graph;
  graph.useLinearRGB = false;
  graph.nodes = {blurNode(false), colorMatrixNode(true), blurNode(false)};

  Pixmap actual = source;
  ASSERT_TRUE(executeFilterGraph(actual, graph));

  const Pixmap expected = runChainWithPerStepConversions(source, {false, true, false});
  EXPECT_TRUE(PixmapsEqual(actual, expected));
}

// A graph that interpolates entirely in sRGB touches the transfer function nowhere at all.
TEST(FilterGraphColorSpace, SrgbGraphNeverConverts) {
  const Pixmap source = makeSourceGraphic();

  FilterGraph graph;
  graph.useLinearRGB = false;
  graph.nodes = {blurNode(false), colorMatrixNode(false), blurNode(false)};

  Pixmap actual = source;
  ASSERT_TRUE(executeFilterGraph(actual, graph));

  const Pixmap expected = runChainWithPerStepConversions(source, {false, false, false});
  EXPECT_TRUE(PixmapsEqual(actual, expected));
}

// feFlood authors its color in sRGB, so a linearRGB primitive has to produce the converted color.
// The executor converts the color and then fills; the reference below fills and then converts the
// whole buffer, which is the conversion the per-primitive code ran. Every pixel of a flood holds
// the same value, so the two must agree exactly.
TEST(FilterGraphColorSpace, FloodColorReachesALinearConsumerConverted) {
  const Pixmap source = makeSourceGraphic();

  GraphNode floodNode;
  floodNode.primitive = graph_primitive::Flood{kFloodR, kFloodG, kFloodB, kFloodA};
  floodNode.useLinearRGB = true;
  floodNode.result = "flood";

  GraphNode mergeNode;
  mergeNode.primitive = graph_primitive::Merge{};
  mergeNode.inputs = {NodeInput(NodeInput::Named{"flood"}),
                      NodeInput(StandardInput::SourceGraphic)};
  mergeNode.useLinearRGB = true;

  FilterGraph graph;
  graph.useLinearRGB = true;
  graph.nodes = {std::move(floodNode), std::move(mergeNode)};

  Pixmap actual = source;
  ASSERT_TRUE(executeFilterGraph(actual, graph));

  auto maybeFlood = FloatPixmap::fromSize(kWidth, kHeight);
  ASSERT_TRUE(maybeFlood.has_value());
  FloatPixmap floodLayer = std::move(*maybeFlood);
  flood(floodLayer, kFloodR / 255.0f, kFloodG / 255.0f, kFloodB / 255.0f, kFloodA / 255.0f);
  srgbToLinear(floodLayer);

  FloatPixmap sourceLayer = FloatPixmap::fromPixmap(source);
  srgbToLinear(sourceLayer);

  auto maybeMerged = FloatPixmap::fromSize(kWidth, kHeight);
  ASSERT_TRUE(maybeMerged.has_value());
  FloatPixmap merged = std::move(*maybeMerged);
  const std::vector<const FloatPixmap*> layers = {&floodLayer, &sourceLayer};
  merge(std::span<const FloatPixmap* const>(layers), merged);
  linearToSrgb(merged);

  EXPECT_TRUE(PixmapsEqual(actual, merged.toPixmap()));
}

// The per-pixel conversion the flood path relies on has to be the same computation the buffer
// conversion applies, for colors at both ends of the transfer function and at partial alpha.
TEST(FilterGraphColorSpace, PerPixelConversionMatchesBufferConversion) {
  const std::array<std::array<std::uint8_t, 4>, 6> colors = {{{0, 0, 0, 0},
                                                              {255, 255, 255, 255},
                                                              {kFloodR, kFloodG, kFloodB, kFloodA},
                                                              {1, 2, 3, 255},
                                                              {10, 10, 10, 12},
                                                              {90, 200, 40, 128}}};

  for (const auto& color : colors) {
    const std::array<float, 4> premultiplied = {color[0] / 255.0f, color[1] / 255.0f,
                                                color[2] / 255.0f, color[3] / 255.0f};

    auto maybeFilled = FloatPixmap::fromSize(4, 4);
    ASSERT_TRUE(maybeFilled.has_value());
    FloatPixmap filled = std::move(*maybeFilled);
    flood(filled, premultiplied[0], premultiplied[1], premultiplied[2], premultiplied[3]);
    srgbToLinear(filled);

    const std::array<float, 4> converted = srgbToLinearPixel(premultiplied);
    const auto filledData = filled.data();
    for (int channel = 0; channel < 4; ++channel) {
      EXPECT_EQ(converted[channel], filledData[channel])
          << "channel " << channel << " of color " << static_cast<int>(color[0]) << ","
          << static_cast<int>(color[1]) << "," << static_cast<int>(color[2]) << ","
          << static_cast<int>(color[3]);
    }
  }
}

}  // namespace
}  // namespace tiny_skia::filter
