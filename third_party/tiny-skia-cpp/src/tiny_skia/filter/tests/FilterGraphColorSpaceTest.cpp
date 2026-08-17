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
#include "tiny_skia/filter/Composite.h"
#include "tiny_skia/filter/FilterGraph.h"
#include "tiny_skia/filter/FloatPixmap.h"
#include "tiny_skia/filter/Flood.h"
#include "tiny_skia/filter/GaussianBlur.h"
#include "tiny_skia/filter/Merge.h"
#include "tiny_skia/filter/Morphology.h"
#include "tiny_skia/filter/Offset.h"
#include "tiny_skia/filter/Tile.h"
#include "tiny_skia/filter/Turbulence.h"

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

/// A transparent working buffer at the test size.
FloatPixmap blankFloat() {
  auto maybeBlank = FloatPixmap::fromSize(kWidth, kHeight);
  EXPECT_TRUE(maybeBlank.has_value());
  return std::move(*maybeBlank);
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

  FloatPixmap floodLayer = blankFloat();
  flood(floodLayer, kFloodR / 255.0f, kFloodG / 255.0f, kFloodB / 255.0f, kFloodA / 255.0f);
  srgbToLinear(floodLayer);

  FloatPixmap sourceLayer = FloatPixmap::fromPixmap(source);
  srgbToLinear(sourceLayer);

  FloatPixmap merged = blankFloat();
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

// ---------------------------------------------------------------------------
// Buffers consumed in both spaces, and primitives that carry their input's space
// ---------------------------------------------------------------------------
//
// The cases above pin where conversions land along a chain. These pin the two claims a chain
// cannot reach: that a buffer read in both spaces stays correct in each, and that a primitive
// which only moves or copies pixels carries its producer's space instead of converting to its
// own. A mis-tagged pass-through reinterprets one space's values as the other, which is a tens-
// of-units error rather than a rounding one, and no chained test notices because every stage
// still runs.

// A result consumed in one space by one primitive and the other space by another must be correct
// in both. Converting the stored pixels in place, or caching the conversion under the wrong tag,
// would feed the second consumer values from the wrong space.
TEST(FilterGraphColorSpace, BufferConsumedInBothSpacesStaysCorrectInEach) {
  const Pixmap source = makeSourceGraphic();

  GraphNode blurLinear = blurNode(true);
  blurLinear.inputs = {NodeInput(StandardInput::SourceGraphic)};
  blurLinear.result = "blurred";

  GraphNode matrixSrgb = colorMatrixNode(false);
  matrixSrgb.inputs = {NodeInput(NodeInput::Named{"blurred"})};
  matrixSrgb.result = "saturated";

  GraphNode mergeLinear;
  mergeLinear.primitive = graph_primitive::Merge{};
  mergeLinear.inputs = {NodeInput(NodeInput::Named{"blurred"}),
                        NodeInput(NodeInput::Named{"saturated"})};
  mergeLinear.useLinearRGB = true;

  FilterGraph graph;
  graph.useLinearRGB = true;
  graph.nodes = {std::move(blurLinear), std::move(matrixSrgb), std::move(mergeLinear)};

  Pixmap actual = source;
  ASSERT_TRUE(executeFilterGraph(actual, graph));

  // Reference: the per-primitive schedule, every result normalized back to sRGB after its node.
  FloatPixmap blurred = FloatPixmap::fromPixmap(source);
  srgbToLinear(blurred);
  gaussianBlur(blurred, kSigma, kSigma, BlurEdgeMode::None);
  linearToSrgb(blurred);

  FloatPixmap saturated(blurred);
  colorMatrix(saturated, saturateMatrix());  // sRGB primitive, no conversion.

  FloatPixmap blurredLinear(blurred);
  srgbToLinear(blurredLinear);
  FloatPixmap saturatedLinear(saturated);
  srgbToLinear(saturatedLinear);

  FloatPixmap merged = blankFloat();
  const std::vector<const FloatPixmap*> layers = {&blurredLinear, &saturatedLinear};
  merge(std::span<const FloatPixmap* const>(layers), merged);
  linearToSrgb(merged);

  EXPECT_LE(maxChannelDelta(actual, merged.toPixmap()), 2);
}

/// Runs `blurred = linearRGB blur of the source`, then `trailing`, and compares against the same
/// blur with no further conversion: a primitive that only moves or copies pixels must leave its
/// producer's space alone, even when the primitive itself is tagged with the other space.
void ExpectCarriesProducerSpace(GraphNode trailing, const FloatPixmap& expected) {
  const Pixmap source = makeSourceGraphic();

  GraphNode blurLinear = blurNode(true);
  blurLinear.inputs = {NodeInput(StandardInput::SourceGraphic)};

  FilterGraph graph;
  graph.useLinearRGB = true;
  graph.nodes = {std::move(blurLinear), std::move(trailing)};

  Pixmap actual = source;
  ASSERT_TRUE(executeFilterGraph(actual, graph));

  EXPECT_TRUE(PixmapsEqual(actual, expected.toPixmap()));
}

/// The linearRGB blur every pass-through case below feeds from, expressed in sRGB the way the
/// graph's exit conversion delivers it.
FloatPixmap blurredSourceAsSrgb() {
  FloatPixmap fp = FloatPixmap::fromPixmap(makeSourceGraphic());
  srgbToLinear(fp);
  gaussianBlur(fp, kSigma, kSigma, BlurEdgeMode::None);
  linearToSrgb(fp);
  return fp;
}

// feOffset moves pixels and nothing else, so it carries its producer's space even when its own
// color-interpolation-filters says otherwise. Tagging the moved pixels with the primitive's space
// instead would reinterpret linearRGB values as sRGB.
TEST(FilterGraphColorSpace, OffsetCarriesProducerSpaceThrough) {
  GraphNode offsetSrgb;
  offsetSrgb.primitive = graph_primitive::Offset{3, -2};
  offsetSrgb.inputs = {NodeInput()};
  offsetSrgb.useLinearRGB = false;

  FloatPixmap expected = blankFloat();
  filter::offset(blurredSourceAsSrgb(), expected, 3, -2);

  ExpectCarriesProducerSpace(std::move(offsetSrgb), expected);
}

// A zero radius disables feMorphology, and the disabled result is the input unchanged, so it too
// carries the producer's space.
TEST(FilterGraphColorSpace, DisabledMorphologyCarriesProducerSpaceThrough) {
  GraphNode morphDisabled;
  morphDisabled.primitive = graph_primitive::Morphology{MorphologyOp::Erode, 0, 0};
  morphDisabled.inputs = {NodeInput()};
  morphDisabled.useLinearRGB = false;

  ExpectCarriesProducerSpace(std::move(morphDisabled), blurredSourceAsSrgb());
}

// The identity matrix short-circuits feColorMatrix into a pass-through, with the same obligation.
TEST(FilterGraphColorSpace, IdentityColorMatrixCarriesProducerSpaceThrough) {
  GraphNode identitySrgb;
  identitySrgb.primitive = graph_primitive::ColorMatrix{identityMatrix()};
  identitySrgb.inputs = {NodeInput()};
  identitySrgb.useLinearRGB = false;

  ExpectCarriesProducerSpace(std::move(identitySrgb), blurredSourceAsSrgb());
}

// feTile replicates a subregion, which is also a pure pixel move.
TEST(FilterGraphColorSpace, TileCarriesProducerSpaceThrough) {
  const Pixmap source = makeSourceGraphic();

  GraphNode blurLinear = blurNode(true);
  blurLinear.inputs = {NodeInput(StandardInput::SourceGraphic)};
  blurLinear.subregion = PixelRect{0, 0, 8, 8};
  blurLinear.result = "patch";

  GraphNode tileSrgb;
  tileSrgb.primitive = graph_primitive::Tile{};
  tileSrgb.inputs = {NodeInput(NodeInput::Named{"patch"})};
  tileSrgb.useLinearRGB = false;

  FilterGraph graph;
  graph.useLinearRGB = true;
  graph.nodes = {std::move(blurLinear), std::move(tileSrgb)};

  Pixmap actual = source;
  ASSERT_TRUE(executeFilterGraph(actual, graph));

  FloatPixmap patch = blurredSourceAsSrgb();
  // The executor clips a result to its subregion before publishing it.
  auto patchData = patch.data();
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      if (x < 8 && y < 8) {
        continue;
      }
      const std::size_t offset = static_cast<std::size_t>((y * kWidth + x) * 4);
      patchData[offset + 0] = 0.0f;
      patchData[offset + 1] = 0.0f;
      patchData[offset + 2] = 0.0f;
      patchData[offset + 3] = 0.0f;
    }
  }

  FloatPixmap tiled = blankFloat();
  tile(patch, tiled, 0, 0, 8, 8);

  EXPECT_TRUE(PixmapsEqual(actual, tiled.toPixmap()));
}

// The control for the pass-through cases: an enabled feMorphology is not a pure pixel move, since
// a channel minimum depends on the space the channels are in, so a disagreeing primitive must
// convert. If pass-through carrying were applied here the result would keep the producer's space
// and this would not match.
TEST(FilterGraphColorSpace, EnabledMorphologyConvertsWhenSpacesDisagree) {
  GraphNode erodeSrgb;
  erodeSrgb.primitive = graph_primitive::Morphology{MorphologyOp::Erode, 2, 2};
  erodeSrgb.inputs = {NodeInput()};
  erodeSrgb.useLinearRGB = false;

  FloatPixmap expected = blankFloat();
  morphology(blurredSourceAsSrgb(), expected, MorphologyOp::Erode, 2, 2);

  ExpectCarriesProducerSpace(std::move(erodeSrgb), expected);
}

// SourceAlpha has zero RGB, so both transfer functions are the identity on it and it must reach a
// linearRGB primitive without a conversion pass. The reference applies the conversion the
// per-primitive code ran, which has to be an exact no-op here.
TEST(FilterGraphColorSpace, SourceAlphaIsSpaceInvariant) {
  const Pixmap source = makeSourceGraphic();

  GraphNode blurLinear = blurNode(true);
  blurLinear.inputs = {NodeInput(StandardInput::SourceAlpha)};

  FilterGraph graph;
  graph.useLinearRGB = true;
  graph.nodes = {std::move(blurLinear)};

  Pixmap actual = source;
  ASSERT_TRUE(executeFilterGraph(actual, graph));

  FloatPixmap fp = FloatPixmap::fromPixmap(source);
  auto data = fp.data();
  for (int i = 0; i < kWidth * kHeight; ++i) {
    data[i * 4 + 0] = 0.0f;
    data[i * 4 + 1] = 0.0f;
    data[i * 4 + 2] = 0.0f;
  }
  srgbToLinear(fp);
  gaussianBlur(fp, kSigma, kSigma, BlurEdgeMode::None);
  linearToSrgb(fp);

  EXPECT_TRUE(PixmapsEqual(actual, fp.toPixmap()));
}

// feTurbulence generates noise directly in the primitive's space. The per-primitive code generated
// the same values and immediately encoded them as sRGB for storage, so as the last primitive the
// two arrangements have to agree exactly.
TEST(FilterGraphColorSpace, TurbulenceGeneratesDirectlyInTheNodeSpace) {
  const Pixmap source = makeSourceGraphic();

  TurbulenceParams params;
  params.type = TurbulenceType::Turbulence;
  params.baseFrequencyX = 0.05;
  params.baseFrequencyY = 0.05;
  params.numOctaves = 3;
  params.seed = 7.0;

  GraphNode turbulenceLinear;
  turbulenceLinear.primitive = graph_primitive::Turbulence{params};
  turbulenceLinear.useLinearRGB = true;

  FilterGraph graph;
  graph.useLinearRGB = true;
  graph.nodes = {std::move(turbulenceLinear)};

  Pixmap actual = source;
  ASSERT_TRUE(executeFilterGraph(actual, graph));

  FloatPixmap fp = blankFloat();
  turbulence(fp, params);
  linearToSrgb(fp);

  EXPECT_TRUE(PixmapsEqual(actual, fp.toPixmap()));
}

// feDropShadow decomposes into flood, composite-in, offset, blur, and merge. Running all of them
// in the primitive's space, and converting the flood color rather than the flood buffer, has to
// reproduce the old decomposition byte for byte.
TEST(FilterGraphColorSpace, DropShadowInLinearMatchesTheDecomposedSchedule) {
  const Pixmap source = makeSourceGraphic();
  constexpr int kDx = 4;
  constexpr int kDy = 4;

  GraphNode shadow;
  shadow.primitive =
      graph_primitive::DropShadow{kFloodR, kFloodG, kFloodB, kFloodA, kDx, kDy, kSigma, kSigma};
  shadow.inputs = {NodeInput(StandardInput::SourceGraphic)};
  shadow.useLinearRGB = true;

  FilterGraph graph;
  graph.useLinearRGB = true;
  graph.nodes = {std::move(shadow)};

  Pixmap actual = source;
  ASSERT_TRUE(executeFilterGraph(actual, graph));

  FloatPixmap floodLayer = blankFloat();
  flood(floodLayer, kFloodR / 255.0f, kFloodG / 255.0f, kFloodB / 255.0f, kFloodA / 255.0f);
  srgbToLinear(floodLayer);

  FloatPixmap sourceAlpha = FloatPixmap::fromPixmap(source);
  auto sourceAlphaData = sourceAlpha.data();
  for (int i = 0; i < kWidth * kHeight; ++i) {
    sourceAlphaData[i * 4 + 0] = 0.0f;
    sourceAlphaData[i * 4 + 1] = 0.0f;
    sourceAlphaData[i * 4 + 2] = 0.0f;
  }

  FloatPixmap shadowLayer = blankFloat();
  composite(floodLayer, sourceAlpha, shadowLayer, CompositeOp::In);

  FloatPixmap offsetShadow = blankFloat();
  filter::offset(shadowLayer, offsetShadow, kDx, kDy);
  gaussianBlur(offsetShadow, kSigma, kSigma);

  FloatPixmap sourceLinear = FloatPixmap::fromPixmap(source);
  srgbToLinear(sourceLinear);

  FloatPixmap merged = blankFloat();
  const std::vector<const FloatPixmap*> layers = {&offsetShadow, &sourceLinear};
  merge(std::span<const FloatPixmap* const>(layers), merged);
  linearToSrgb(merged);

  EXPECT_TRUE(PixmapsEqual(actual, merged.toPixmap()));
}

}  // namespace
}  // namespace tiny_skia::filter
