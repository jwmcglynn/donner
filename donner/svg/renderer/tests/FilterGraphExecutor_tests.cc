#include "donner/svg/renderer/FilterGraphExecutor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <ostream>
#include <utility>
#include <vector>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/components/animation/AnimationValueSampler.h"
#include "donner/svg/renderer/FilterRegionContext.h"

namespace donner::svg {

// Printers for the input buffer layout structs, so a failed match shows the whole layout.
void PrintTo(const FilterInputAxis& axis, std::ostream* os) {
  *os << "FilterInputAxis{before=" << axis.before << ", extent=" << axis.extent << "}";
}

void PrintTo(const FilterInputBuffer& buffer, std::ostream* os) {
  *os << "FilterInputBuffer{width=" << buffer.width << ", height=" << buffer.height
      << ", offsetX=" << buffer.offsetX << ", offsetY=" << buffer.offsetY << "}";
}

namespace {

using ::testing::_;
using ::testing::AllOf;
using ::testing::ElementsAre;
using ::testing::ElementsAreArray;
using ::testing::Ge;
using ::testing::Gt;
using ::testing::Le;
using ::testing::Lt;

using Pixel = std::array<std::uint8_t, 4>;

MATCHER_P4(Rgba, rMatcher, gMatcher, bMatcher, aMatcher, "has RGBA channels") {
  const std::array<int, 4> channels = {static_cast<int>(arg[0]), static_cast<int>(arg[1]),
                                       static_cast<int>(arg[2]), static_cast<int>(arg[3])};
  return testing::ExplainMatchResult(ElementsAre(rMatcher, gMatcher, bMatcher, aMatcher), channels,
                                     result_listener);
}

auto ChannelNear(int expected, int tolerance) {
  return AllOf(Ge(expected - tolerance), Le(expected + tolerance));
}

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

tiny_skia::Pixmap CreateBlurredDotPixmap(const Transform2d& deviceFromFilterTransform) {
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

  ApplyFilterGraphToPixmap(pixmap, graph, deviceFromFilterTransform, std::nullopt);
  return pixmap;
}

TEST(FilterGraphExecutorTest, AnimatedDefaultRegionUsesExecutedRasterHistory) {
  for (const bool shear : {false, true}) {
    SCOPED_TRACE(shear);
    components::FilterGraph graph;
    graph.filterRegion = Box2d::FromXYWH(0, 0, 32, 32);
    components::FilterNode flood;
    flood.primitive = components::filter_primitive::Flood{};
    flood.x = Lengthd(10);
    flood.y = Lengthd(10);
    flood.width = Lengthd(8);
    flood.height = Lengthd(8);
    graph.nodes.push_back(flood);
    components::FilterNode blur;
    blur.inputs.emplace_back();
    blur.primitive = components::filter_primitive::GaussianBlur{1, 1};
    graph.nodes.push_back(blur);
    components::FilterNode offset;
    offset.inputs.emplace_back();
    offset.primitive = components::filter_primitive::Offset{};
    components::AnimateValueComponent value;
    value.attributeName = "width";
    value.to = "6";
    offset.animatedRegion[2] = components::ContextualLengthRecipe::Create({{value, 0.25, 0}});
    ASSERT_THAT(offset.animatedRegion[2], testing::NotNull());
    graph.nodes.push_back(offset);
    components::FilterGraph expectedGraph = graph;
    auto& expectedNode = expectedGraph.nodes.back();
    expectedNode.animatedRegion = {};
    expectedNode.x = Lengthd(shear ? 1.5 : 7);
    expectedNode.y = Lengthd(7);
    expectedNode.width = Lengthd(shear ? 20.25 : 12);
    expectedNode.height = Lengthd(14);
    auto actual = tiny_skia::Pixmap::fromSize(64, 64);
    auto expected = tiny_skia::Pixmap::fromSize(64, 64);
    ASSERT_THAT(actual, testing::Ne(std::nullopt));
    ASSERT_THAT(expected, testing::Ne(std::nullopt));
    Transform2d deviceFromFilter = Transform2d::Scale(shear ? 1 : 2);
    if (shear) {
      deviceFromFilter.data[2] = 0.5;
    }
    ApplyFilterGraphToPixmap(*actual, graph, deviceFromFilter, graph.filterRegion);
    ApplyFilterGraphToPixmap(*expected, expectedGraph, deviceFromFilter, graph.filterRegion);
    editor::tests::CompareBitmapToBitmap(
        RendererBitmap{Vector2i(64, 64),
                       std::vector<uint8_t>(actual->data().begin(), actual->data().end()), 64 * 4},
        RendererBitmap{Vector2i(64, 64),
                       std::vector<uint8_t>(expected->data().begin(), expected->data().end()),
                       64 * 4},
        shear ? "animated-default-shear" : "animated-default-scale",
        editor::tests::PixelmatchIdentityParams());
  }
}

TEST(FilterGraphExecutorTest, EmptyInputRegionDoesNotBecomePositiveDuringSampling) {
  components::FilterGraph graph;
  const Box2d clip = Box2d::FromXYWH(0, 0, 16, 16);
  FilterRegionContext regions(graph, Vector2i(16, 16), clip, Transform2d(), clip, clip);
  components::FilterNode flood;
  flood.x = Lengthd(20);
  flood.y = Lengthd(0);
  flood.width = Lengthd(8);
  flood.height = Lengthd(8);
  flood.primitive = components::filter_primitive::Flood{};
  regions.recordRasterSubregion(flood, Box2d::FromXYWH(20, 0, 8, 8));
  components::FilterNode offset;
  offset.inputs.emplace_back();
  offset.primitive = components::filter_primitive::Offset{};
  components::AnimateValueComponent value;
  value.attributeName = "width";
  value.to = "4";
  offset.animatedRegion[2] = components::ContextualLengthRecipe::Create({{value, 0.5, 0}});
  const auto sampled = regions.sampleNodeSubregion(offset);
  ASSERT_THAT(sampled.animatedRegion, testing::Ne(std::nullopt));
  EXPECT_DOUBLE_EQ(sampled.animatedRegion->width(), 2);
}

TEST(FilterGraphExecutorTest, SingularContextDoesNotPublishAnimatedGeometry) {
  components::FilterGraph graph;
  const Box2d clip = Box2d::FromXYWH(0, 0, 16, 16);
  components::FilterNode node;
  components::AnimateValueComponent value;
  value.attributeName = "width";
  value.from = "4";
  value.to = "8";
  node.animatedRegion[2] = components::ContextualLengthRecipe::Create({{value, 0.5, 0}});
  std::array<Transform2d, 4> transforms;
  transforms[0] = Transform2d::Scale(0);
  transforms[1].data[4] = std::numeric_limits<double>::infinity();
  transforms[2].data[5] = std::numeric_limits<double>::quiet_NaN();
  transforms[3].data[0] = 1e-6;
  transforms[3].data[3] = 1e-5;
  transforms[3].data[4] = 1e308;
  for (std::size_t i = 0; i < transforms.size(); ++i) {
    SCOPED_TRACE(i);
    FilterRegionContext regions(graph, Vector2i(16, 16), clip, transforms[i], clip, clip);
    const auto sampled = regions.sampleNodeSubregion(node);
    EXPECT_THAT(sampled.animatedRegion, testing::Eq(std::nullopt));
    EXPECT_THAT(sampled.bounds, testing::Eq(Box2d::FromXYWH(0, 0, 0, 0)));
  }
}

TEST(FilterGraphExecutorTest, AnimatedPaintInputDefaultsUsePaintCoverage) {
  for (const int mode : {0, 1, 2}) {
    SCOPED_TRACE(mode);
    const bool stroke = mode == 1;
    components::FilterGraph graph;
    graph.filterRegion = Box2d::FromXYWH(0, 0, 16, 16);
    components::FilterNode offset;
    offset.primitive = components::filter_primitive::Offset{};
    offset.inputs.emplace_back(stroke ? components::FilterStandardInput::StrokePaint
                                      : components::FilterStandardInput::FillPaint);
    components::AnimateValueComponent value;
    value.attributeName = "width";
    value.to = "4";
    offset.animatedRegion[2] = components::ContextualLengthRecipe::Create({{value, 0.25, 0}});
    graph.nodes.push_back(offset);
    components::FilterGraph expectedGraph = graph;
    auto& expectedNode = expectedGraph.nodes.back();
    expectedNode.animatedRegion = {};
    expectedNode.x = Lengthd(4);
    expectedNode.y = Lengthd(4);
    expectedNode.width = Lengthd(7);
    expectedNode.height = Lengthd(8);
    auto paint = tiny_skia::Pixmap::fromSize(16, 16);
    auto actual = tiny_skia::Pixmap::fromSize(16, 16);
    auto expected = tiny_skia::Pixmap::fromSize(16, 16);
    ASSERT_THAT(paint, testing::Ne(std::nullopt));
    ASSERT_THAT(actual, testing::Ne(std::nullopt));
    ASSERT_THAT(expected, testing::Ne(std::nullopt));
    for (int y = 4; y < 12; ++y) {
      for (int x = 4; x < 12; ++x) {
        SetPixel(*paint, x, y, Pixel{255, 0, 0, 255});
      }
    }
    const auto* fillInput = mode == 1 ? nullptr : &*paint;
    const auto* strokeInput = mode == 0 ? nullptr : &*paint;
    ApplyFilterGraphToPixmap(*actual, graph, Transform2d(), graph.filterRegion, false, fillInput,
                             strokeInput);
    ApplyFilterGraphToPixmap(*expected, expectedGraph, Transform2d(), graph.filterRegion, false,
                             fillInput, strokeInput);
    editor::tests::CompareBitmapToBitmap(
        RendererBitmap{Vector2i(16, 16),
                       std::vector<uint8_t>(actual->data().begin(), actual->data().end()), 16 * 4},
        RendererBitmap{Vector2i(16, 16),
                       std::vector<uint8_t>(expected->data().begin(), expected->data().end()),
                       16 * 4},
        stroke ? "animated-stroke-region" : "animated-fill-region",
        editor::tests::PixelmatchIdentityParams());
  }
}

TEST(FilterGraphExecutorTest, ContextualPaintPreflightRejectsOversizedInputs) {
  for (const bool stroke : {false, true}) {
    SCOPED_TRACE(stroke);
    components::FilterGraph graph;
    components::FilterNode node;
    components::AnimateValueComponent value;
    value.attributeName = "width";
    value.to = "8";
    node.animatedRegion[2] = components::ContextualLengthRecipe::Create({{value, 0.5, 0}});
    graph.nodes.push_back(node);
    auto pixmap = tiny_skia::Pixmap::fromSize(16, 16);
    auto paint = tiny_skia::Pixmap::fromSize(32, 16);
    ASSERT_THAT(pixmap, testing::Ne(std::nullopt));
    ASSERT_THAT(paint, testing::Ne(std::nullopt));
    components::FilterExecutionBudget budget;
    ApplyFilterGraphToPixmap(*pixmap, graph, Transform2d(), std::nullopt, false,
                             stroke ? nullptr : &*paint, stroke ? &*paint : nullptr, &budget);
    EXPECT_THAT(budget.rejectionReason(),
                testing::Eq(components::FilterExecutionBudget::RejectionReason::External));
  }
}

TEST(FilterGraphExecutorTest, ContextualWorkIncludesEveryExecutionAndPaintScan) {
  components::FilterGraph graph;
  components::FilterNode node;
  node.primitive = components::filter_primitive::Flood{};
  components::AnimateValueComponent value;
  value.attributeName = "width";
  value.values = {"100%", "4px", "0.5"};
  node.animatedRegion[2] = components::ContextualLengthRecipe::Create({{value, 0.5, 0}});
  graph.nodes.push_back(node);
  uint64_t singleWork = 0;
  uint64_t repeatedWork = 0;
  uint64_t bytes = 0;
  ASSERT_THAT(
      components::FilterGraphExecutionCost(
          graph, 256, components::FilterMemoryModel::CpuFloatNamedResults, singleWork, bytes),
      testing::IsTrue());
  ASSERT_THAT(components::FilterGraphExecutionCost(
                  graph, 1024, components::FilterMemoryModel::CpuFloatNamedResults, repeatedWork,
                  bytes, 256, 4),
              testing::IsTrue());
  EXPECT_EQ(repeatedWork, singleWork * 4);
  const auto recipeWork = node.animatedRegion[2]->evaluationWork;
  graph.nodes[0].animatedRegion = {};
  uint64_t baseWork = 0;
  ASSERT_THAT(components::FilterGraphExecutionCost(
                  graph, 256, components::FilterMemoryModel::CpuFloatNamedResults, baseWork, bytes),
              testing::IsTrue());
  EXPECT_THAT(singleWork, testing::Ge(baseWork + recipeWork + 2 * 256));
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

  ApplyFilterGraphToPixmap(pixmap, graph, Transform2d::Scale(2.0), std::nullopt);

  EXPECT_THAT(GetPixel(pixmap, 1, 1), Rgba(0, 0, 0, 0));
  EXPECT_THAT(GetPixel(pixmap, 3, 1), Rgba(255, 0, 0, 255));
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

  ApplyFilterGraphToPixmap(pixmap, graph, Transform2d::Scale(2.5), std::nullopt);

  // dx=5.0 with Scale(2.5) → pixel offset = lround(12.5) = 13.
  // Pixel at (1,1) moves to (14,1).
  EXPECT_THAT(GetPixel(pixmap, 14, 1), Rgba(255, 0, 0, 255));
  EXPECT_THAT(GetPixel(pixmap, 13, 1), Rgba(0, 0, 0, 0));
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

  ClipFilterOutputToRegion(pixmap, Box2d::FromXYWH(1.0, 1.0, 2.0, 2.0), Transform2d::Scale(2.0));

  EXPECT_THAT(GetPixel(pixmap, 1, 1), Rgba(0, 0, 0, 0));
  EXPECT_THAT(GetPixel(pixmap, 2, 2), Rgba(255, 255, 255, 255));
  EXPECT_THAT(GetPixel(pixmap, 5, 5), Rgba(255, 255, 255, 255));
  EXPECT_THAT(GetPixel(pixmap, 6, 6), Rgba(0, 0, 0, 0));
}

TEST(FilterGraphExecutorTest, ClipsFractionalAxisAlignedRegionByPixelCenter) {
  auto maybePixmap = tiny_skia::Pixmap::fromSize(4, 4);
  ASSERT_TRUE(maybePixmap.has_value());
  tiny_skia::Pixmap pixmap = std::move(*maybePixmap);

  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      SetPixel(pixmap, x, y, Pixel{255, 255, 255, 255});
    }
  }

  ClipFilterOutputToRegion(pixmap, Box2d::FromXYWH(0.6, 1.4, 2.0, 2.0), Transform2d());

  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      const bool pixelCenterInside = x >= 1 && x <= 2 && y >= 1 && y <= 2;
      EXPECT_THAT(GetPixel(pixmap, x, y),
                  pixelCenterInside ? Rgba(255, 255, 255, 255) : Rgba(0, 0, 0, 0))
          << "at " << x << "," << y;
    }
  }
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

  ClipFilterOutputToRegion(pixmap, Box2d::FromXYWH(-10.0, -10.0, 2.0, 2.0), Transform2d());

  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      EXPECT_THAT(GetPixel(pixmap, x, y), Rgba(0, 0, 0, 0));
    }
  }
}

TEST(FilterGraphExecutorTest, ExtremeFilterValuesStayWithinNumericAndWorkBounds) {
  auto maybePixmap = tiny_skia::Pixmap::fromSize(8, 8);
  ASSERT_TRUE(maybePixmap.has_value());
  tiny_skia::Pixmap pixmap = std::move(*maybePixmap);
  SetPixel(pixmap, 1, 1, Pixel{255, 0, 0, 255});

  components::FilterGraph graph;
  components::FilterNode offset;
  offset.inputs.push_back(components::FilterInput{});
  offset.primitive = components::filter_primitive::Offset{.dx = 1e300, .dy = -1e300};
  graph.nodes.push_back(std::move(offset));

  components::FilterNode morphology;
  morphology.inputs.push_back(components::FilterInput{});
  morphology.primitive = components::filter_primitive::Morphology{
      .op = components::filter_primitive::Morphology::Operator::Dilate,
      .radiusX = 1e300,
      .radiusY = 1e300,
  };
  graph.nodes.push_back(std::move(morphology));

  ApplyFilterGraphToPixmap(pixmap, graph, Transform2d::Scale(1e300),
                           Box2d::FromXYWH(-1e300, -1e300, 2e300, 2e300));
  ClipFilterOutputToRegion(pixmap, Box2d::FromXYWH(1e300, 1e300, 1e300, 1e300),
                           Transform2d::Scale(1e300));

  EXPECT_EQ(pixmap.width(), 8u);
  EXPECT_EQ(pixmap.height(), 8u);
}

TEST(FilterGraphExecutorTest, MorphologyAdmissionChargesPassSamplesAndRetainedTextures) {
  components::FilterGraph graph;
  for (int index = 0; index < 4; ++index) {
    components::FilterNode node;
    node.primitive = components::filter_primitive::Morphology{
        .op = components::filter_primitive::Morphology::Operator::Dilate,
        .radiusX = components::kMaximumFilterMorphologyRadius,
        .radiusY = components::kMaximumFilterMorphologyRadius,
    };
    graph.nodes.push_back(std::move(node));
  }

  constexpr std::uint64_t kExpectedWorkMultiplier = 1047;
  const std::uint64_t boundaryPixels =
      components::kMaximumFilterWorkUnits / (4 * kExpectedWorkMultiplier);
  EXPECT_TRUE(components::FilterGraphFitsExecutionBudget(
      graph, boundaryPixels, components::FilterMemoryModel::GpuAllNodes));
  EXPECT_FALSE(components::FilterGraphFitsExecutionBudget(
      graph, boundaryPixels + 1, components::FilterMemoryModel::GpuAllNodes));

  std::uint64_t workUnits = 0;
  std::uint64_t retainedBytes = 0;
  ASSERT_TRUE(components::FilterGraphExecutionCost(
      graph, 100, components::FilterMemoryModel::GpuAllNodes, workUnits, retainedBytes));
  EXPECT_EQ(workUnits, 4u * 100u * kExpectedWorkMultiplier);
  const uint64_t largeRadiusBytes = retainedBytes;
  for (auto& node : graph.nodes) {
    auto& morphology = std::get<components::filter_primitive::Morphology>(node.primitive);
    morphology.radiusX = 1;
    morphology.radiusY = 1;
  }
  ASSERT_TRUE(components::FilterGraphExecutionCost(
      graph, 100, components::FilterMemoryModel::GpuAllNodes, workUnits, retainedBytes));
  EXPECT_EQ(retainedBytes, largeRadiusBytes);
}

TEST(FilterGraphExecutorTest, RejectsAggregateConvolveWorkAndNamedBufferMemory) {
  components::FilterGraph graph;
  components::FilterNode convolve;
  convolve.primitive = components::filter_primitive::ConvolveMatrix{
      .orderX = 32,
      .orderY = 32,
      .kernelMatrix = std::vector<double>(32 * 32, 1.0),
  };
  convolve.result = RcString("convolve");
  graph.nodes.push_back(std::move(convolve));
  for (size_t i = 1; i < components::kMaximumFilterGraphNodes; ++i) {
    components::FilterNode flood;
    flood.primitive = components::filter_primitive::Flood{};
    flood.result = RcString("retained-" + std::to_string(i));
    graph.nodes.push_back(std::move(flood));
  }

  constexpr std::uint64_t kPixels = 1024ULL * 1024;
  EXPECT_FALSE(components::FilterGraphFitsExecutionBudget(
      graph, kPixels, components::FilterMemoryModel::CpuFloatNamedResults));
  EXPECT_FALSE(components::FilterGraphFitsExecutionBudget(
      graph, kPixels, components::FilterMemoryModel::GpuAllNodes));
}

TEST(FilterGraphExecutorTest, AccountsForCpuBaselineFilterBuffers) {
  components::FilterGraph graph;
  components::FilterNode flood;
  flood.primitive = components::filter_primitive::Flood{};
  graph.nodes.push_back(std::move(flood));

  EXPECT_FALSE(components::FilterGraphFitsExecutionBudget(
      graph, components::kMaximumFilterSurfacePixels,
      components::FilterMemoryModel::CpuFloatNamedResults));

  components::FilterNode secondFlood;
  secondFlood.primitive = components::filter_primitive::Flood{};
  graph.nodes.push_back(std::move(secondFlood));
  EXPECT_FALSE(components::FilterGraphFitsExecutionBudget(
      graph, components::kMaximumFilterSurfacePixels,
      components::FilterMemoryModel::CpuFloatNamedResults));
}

TEST(FilterGraphExecutorTest, AccountsForTurbulenceOctaveWork) {
  components::FilterGraph graph;
  components::FilterNode turbulence;
  turbulence.primitive = components::filter_primitive::Turbulence{.numOctaves = 16};
  graph.nodes.push_back(std::move(turbulence));

  EXPECT_FALSE(components::FilterGraphFitsExecutionBudget(
      graph, 1024ULL * 1024, components::FilterMemoryModel::CpuFloatNamedResults));
  EXPECT_TRUE(components::FilterGraphFitsExecutionBudget(
      graph, 1024ULL * 1024, components::FilterMemoryModel::GpuAllNodes));
}

TEST(FilterGraphExecutorTest, AccountsForBlurPassWorkAndScratchMemory) {
  components::FilterGraph graph;
  for (int i = 0; i < 21; ++i) {
    components::FilterNode blur;
    blur.primitive = components::filter_primitive::GaussianBlur{
        .stdDeviationX = 3.0,
        .stdDeviationY = 3.0,
    };
    graph.nodes.push_back(std::move(blur));
  }

  constexpr std::uint64_t kExactWorkBudgetPixels = 1024ULL * 1024;
  EXPECT_TRUE(components::FilterGraphFitsExecutionBudget(
      graph, kExactWorkBudgetPixels, components::FilterMemoryModel::CpuFloatNamedResults));
  graph.nodes.push_back(graph.nodes.front());
  EXPECT_FALSE(components::FilterGraphFitsExecutionBudget(
      graph, kExactWorkBudgetPixels, components::FilterMemoryModel::CpuFloatNamedResults));

  graph.nodes.resize(1);
  EXPECT_TRUE(components::FilterGraphFitsExecutionBudget(
      graph, components::kMaximumFilterSurfacePixels,
      components::FilterMemoryModel::CpuFloatNamedResults));
  graph.nodes.push_back(graph.nodes.front());
  EXPECT_FALSE(components::FilterGraphFitsExecutionBudget(
      graph, components::kMaximumFilterSurfacePixels,
      components::FilterMemoryModel::CpuFloatNamedResults));
  EXPECT_FALSE(components::FilterGraphFitsExecutionBudget(
      graph, components::kMaximumFilterSurfacePixels, components::FilterMemoryModel::GpuAllNodes));

  constexpr std::uint64_t kLinearRgbGpuBoundaryPixels = 3'800'000;
  EXPECT_FALSE(components::FilterGraphFitsExecutionBudget(
      graph, kLinearRgbGpuBoundaryPixels, components::FilterMemoryModel::GpuAllNodes));
  graph.colorInterpolationFilters = ColorInterpolationFilters::SRGB;
  EXPECT_FALSE(components::FilterGraphFitsExecutionBudget(
      graph, kLinearRgbGpuBoundaryPixels, components::FilterMemoryModel::GpuAllNodes));
  EXPECT_TRUE(components::FilterGraphFitsExecutionBudget(
      graph, 2'000'000, components::FilterMemoryModel::GpuAllNodes));
}

TEST(FilterGraphExecutorTest, AccountsForGpuPerNodeSubregionTextures) {
  components::FilterGraph graph;
  for (int i = 0; i < 3; ++i) {
    components::FilterNode blend;
    blend.primitive = components::filter_primitive::Blend{};
    graph.nodes.push_back(std::move(blend));
  }

  constexpr std::uint64_t kGpuNodeClipBoundaryPixels = 4'200'000;
  EXPECT_FALSE(components::FilterGraphFitsExecutionBudget(
      graph, kGpuNodeClipBoundaryPixels, components::FilterMemoryModel::GpuAllNodes));
}

TEST(FilterGraphExecutorTest, AccountsForGenericPrimitivePassWork) {
  components::FilterGraph graph;
  graph.colorInterpolationFilters = ColorInterpolationFilters::SRGB;
  for (int i = 0; i < 12; ++i) {
    components::FilterNode blend;
    blend.primitive = components::filter_primitive::Blend{};
    graph.nodes.push_back(std::move(blend));
  }

  constexpr std::uint64_t kGenericWorkBoundaryPixels = 4'800'000;
  EXPECT_FALSE(components::FilterGraphFitsExecutionBudget(
      graph, kGenericWorkBoundaryPixels, components::FilterMemoryModel::CpuFloatNamedResults));
}

TEST(FilterGraphExecutorTest, AccountsForZeroDeviationBlurAndShadowWork) {
  components::FilterGraph zeroBlurGraph;
  for (int i = 0; i < 32; ++i) {
    components::FilterNode blur;
    blur.primitive = components::filter_primitive::GaussianBlur{};
    zeroBlurGraph.nodes.push_back(std::move(blur));
  }
  EXPECT_FALSE(components::FilterGraphFitsExecutionBudget(
      zeroBlurGraph, 2'000'000, components::FilterMemoryModel::CpuFloatNamedResults));

  components::FilterGraph zeroShadowGraph;
  for (int i = 0; i < 16; ++i) {
    components::FilterNode shadow;
    shadow.primitive = components::filter_primitive::DropShadow{};
    zeroShadowGraph.nodes.push_back(std::move(shadow));
  }
  EXPECT_FALSE(components::FilterGraphFitsExecutionBudget(
      zeroShadowGraph, 1'500'000, components::FilterMemoryModel::CpuFloatNamedResults));
}

TEST(FilterGraphExecutorTest, AccountsForBicubicImageSamplingWork) {
  components::FilterGraph graph;
  components::FilterNode imageNode;
  imageNode.primitive = components::filter_primitive::Image{};
  graph.nodes.push_back(std::move(imageNode));

  EXPECT_FALSE(components::FilterGraphFitsExecutionBudget(
      graph, 4'000'000, components::FilterMemoryModel::CpuFloatNamedResults));
  auto& image = std::get<components::filter_primitive::Image>(graph.nodes[0].primitive);
  image.imageRendering = ImageRendering::Pixelated;
  EXPECT_TRUE(components::FilterGraphFitsExecutionBudget(
      graph, 4'000'000, components::FilterMemoryModel::CpuFloatNamedResults));
  EXPECT_TRUE(components::FilterGraphFitsExecutionBudget(
      graph, 4'000'000, components::FilterMemoryModel::GpuAllNodes));
}

TEST(FilterGraphExecutorTest, AccountsForDropShadowBlurWorkAndMemory) {
  components::FilterGraph graph;
  for (int i = 0; i < 13; ++i) {
    components::FilterNode shadow;
    shadow.primitive = components::filter_primitive::DropShadow{
        .stdDeviationX = 3.0,
        .stdDeviationY = 3.0,
    };
    graph.nodes.push_back(std::move(shadow));
  }

  EXPECT_FALSE(components::FilterGraphFitsExecutionBudget(
      graph, 1024ULL * 1024, components::FilterMemoryModel::CpuFloatNamedResults));

  graph.nodes.resize(1);
  EXPECT_FALSE(components::FilterGraphFitsExecutionBudget(
      graph, components::kMaximumFilterSurfacePixels,
      components::FilterMemoryModel::CpuFloatNamedResults));
  EXPECT_FALSE(components::FilterGraphFitsExecutionBudget(
      graph, 7'000'000, components::FilterMemoryModel::GpuAllNodes));
}

TEST(FilterGraphExecutorTest, RetainedGpuParametersSurviveChunkAccounting) {
  using namespace components;
  FilterGraph graph;
  FilterNode node;
  node.primitive = filter_primitive::Flood{};
  graph.nodes.push_back(node);
  FilterExecutionBudget budget;
  constexpr uint64_t kRetained = kMaximumFilterFrameBytes / 2;
  auto first = budget.reserve(graph, 16, FilterMemoryModel::GpuAllNodes, 64, kRetained);
  ASSERT_TRUE(first);
  budget.release(*first);
  EXPECT_GE(budget.retainedGpuBytes(), kRetained);
  EXPECT_FALSE(budget.reserve(graph, 16, FilterMemoryModel::GpuAllNodes, kRetained, kRetained));
  ASSERT_TRUE(budget.beginChunkAfterSubmit());
  EXPECT_EQ(budget.retainedGpuBytes(), kRetained);
  EXPECT_FALSE(budget.reserve(graph, 16, FilterMemoryModel::GpuAllNodes, kRetained, kRetained));
  budget.reset();
  EXPECT_EQ(budget.retainedGpuBytes(), 0u);
  EXPECT_FALSE(budget.reserve(graph, 16, FilterMemoryModel::GpuAllNodes, 0, UINT64_MAX));
}

TEST(FilterGraphExecutorTest, ParameterBlockBoundRejectsOverflow) {
  using namespace components;
  EXPECT_EQ(GpuFilterParameterAllocationBound(0), 0u);
  EXPECT_GE(GpuFilterParameterAllocationBound(1024), kGpuFilterParameterBlockBytes);
  EXPECT_GT(GpuFilterParameterAllocationBound(UINT64_MAX), kMaximumFilterFrameBytes);
}

TEST(FilterGraphExecutorTest, RejectsAggregateWorkAcrossSeparateFilterExecutions) {
  components::FilterGraph graph;
  components::FilterNode blur;
  blur.primitive = components::filter_primitive::GaussianBlur{
      .stdDeviationX = 3.0,
      .stdDeviationY = 3.0,
  };
  graph.nodes.push_back(std::move(blur));

  components::FilterExecutionBudget budget;
  constexpr std::uint64_t kPixels = 1024ULL * 1024;
  for (int i = 0; i < 42; ++i) {
    EXPECT_TRUE(
        budget.consume(graph, kPixels, components::FilterMemoryModel::CpuFloatNamedResults));
  }
  EXPECT_FALSE(budget.consume(graph, kPixels, components::FilterMemoryModel::CpuFloatNamedResults));

  budget.reset();
  std::size_t maximumSurfaceExecutions = 0;
  while (budget.consume(graph, components::kMaximumFilterSurfacePixels,
                        components::FilterMemoryModel::CpuFloatNamedResults)) {
    ++maximumSurfaceExecutions;
  }
  EXPECT_GT(maximumSurfaceExecutions, 0u);
  EXPECT_LT(maximumSurfaceExecutions, components::FilterExecutionBudget::kMaximumExecutions);
  EXPECT_TRUE(budget.rejected());
  EXPECT_LE(budget.workUnits(), components::kMaximumFilterFrameWorkUnits);
}

TEST(FilterGraphExecutorTest, ReservesCaptureMemoryBeforeAllocationAndLatchesRejection) {
  components::FilterGraph graph;
  components::FilterNode blur;
  blur.primitive = components::filter_primitive::GaussianBlur{
      .stdDeviationX = 3.0,
      .stdDeviationY = 3.0,
  };
  graph.nodes.push_back(std::move(blur));

  constexpr std::uint64_t kCaptureBytes = components::kMaximumFilterSurfacePixels * 4;
  components::FilterExecutionBudget cpuBudget;
  auto outer =
      cpuBudget.reserve(graph, components::kMaximumFilterSurfacePixels,
                        components::FilterMemoryModel::CpuFloatNamedResults, kCaptureBytes);
  ASSERT_TRUE(outer.has_value());
  EXPECT_FALSE(cpuBudget
                   .reserve(graph, components::kMaximumFilterSurfacePixels,
                            components::FilterMemoryModel::CpuFloatNamedResults, kCaptureBytes)
                   .has_value());
  EXPECT_TRUE(cpuBudget.rejected());
  EXPECT_FALSE(cpuBudget.reserve(graph, 1, components::FilterMemoryModel::CpuFloatNamedResults, 4)
                   .has_value());

  components::FilterExecutionBudget gpuBudget;
  EXPECT_FALSE(gpuBudget
                   .reserve(graph, components::kMaximumFilterSurfacePixels,
                            components::FilterMemoryModel::GpuAllNodes, kCaptureBytes)
                   .has_value());
}

TEST(FilterGraphExecutorTest, PermanentRejectionCannotStartMemoryChunk) {
  components::FilterGraph invalidGraph;
  invalidGraph.nodes.resize(components::kMaximumFilterGraphNodes + 1u);
  components::FilterExecutionBudget budget;

  EXPECT_FALSE(
      budget.reserve(invalidGraph, 1, components::FilterMemoryModel::GpuAllNodes, 4).has_value());
  EXPECT_EQ(budget.rejectionReason(),
            components::FilterExecutionBudget::RejectionReason::InvalidGraph);
  EXPECT_FALSE(budget.beginChunkAfterSubmit());
  EXPECT_EQ(budget.chunks(), 0u);
}

TEST(FilterGraphExecutorTest, ActiveGpuReservationBlocksMemoryChunk) {
  components::FilterGraph graph;
  components::FilterNode blur;
  blur.primitive = components::filter_primitive::GaussianBlur{
      .stdDeviationX = 3.0,
      .stdDeviationY = 3.0,
  };
  graph.nodes.push_back(std::move(blur));

  constexpr std::uint64_t kPixels = 1024ULL * 1024;
  constexpr std::uint64_t kCaptureBytes = kPixels * 4;
  components::FilterExecutionBudget budget;
  ASSERT_TRUE(
      budget.reserve(graph, kPixels, components::FilterMemoryModel::GpuAllNodes, kCaptureBytes));
  while (
      budget.reserve(graph, kPixels, components::FilterMemoryModel::GpuAllNodes, kCaptureBytes)) {}

  EXPECT_EQ(budget.rejectionReason(),
            components::FilterExecutionBudget::RejectionReason::MemoryLimit);
  EXPECT_GT(budget.activeGpuReservations(), 0u);
  EXPECT_FALSE(budget.beginChunkAfterSubmit());
  EXPECT_EQ(budget.chunks(), 0u);
}

TEST(FilterGraphExecutorTest, EmptyChunkCannotRetryIntrinsicallyOversizedGraph) {
  components::FilterGraph graph;
  components::FilterNode blur;
  blur.primitive = components::filter_primitive::GaussianBlur{
      .stdDeviationX = 3.0,
      .stdDeviationY = 3.0,
  };
  graph.nodes.push_back(std::move(blur));

  components::FilterExecutionBudget budget;
  auto small = budget.reserve(graph, 1024, components::FilterMemoryModel::GpuAllNodes, 4096);
  ASSERT_TRUE(small.has_value());
  budget.release(*small);

  constexpr std::uint64_t kOversizedPixels = 1024;
  constexpr std::uint64_t kCaptureBytes = components::kMaximumFilterFrameBytes;
  EXPECT_FALSE(budget
                   .reserve(graph, kOversizedPixels, components::FilterMemoryModel::GpuAllNodes,
                            kCaptureBytes)
                   .has_value());
  EXPECT_GT(budget.retainedGpuBytes(), 0u);
  ASSERT_TRUE(budget.beginChunkAfterSubmit());
  EXPECT_EQ(budget.chunks(), 1u);

  EXPECT_FALSE(budget
                   .reserve(graph, kOversizedPixels, components::FilterMemoryModel::GpuAllNodes,
                            kCaptureBytes)
                   .has_value());
  EXPECT_EQ(budget.retainedGpuBytes(), 0u);
  EXPECT_FALSE(budget.beginChunkAfterSubmit());
  EXPECT_EQ(budget.chunks(), 1u);
}

TEST(FilterGraphExecutorTest, RejectsNestedCaptureMemoryBeforeAllocation) {
  components::FilterGraph graph;
  components::FilterNode dropShadow;
  dropShadow.primitive = components::filter_primitive::DropShadow{
      .stdDeviationX = 0.1,
      .stdDeviationY = 0.1,
  };
  graph.nodes.push_back(std::move(dropShadow));

  constexpr std::uint64_t kPixels = 1024ULL * 1024;
  constexpr std::uint64_t kCaptureBytes = kPixels * 4;

  components::FilterExecutionBudget cpuBudget;
  std::vector<components::FilterExecutionBudget::Reservation> cpuReservations;
  while (true) {
    auto reservation = cpuBudget.reserve(
        graph, kPixels, components::FilterMemoryModel::CpuFloatNamedResults, kCaptureBytes);
    if (!reservation.has_value()) {
      break;
    }
    cpuReservations.push_back(*reservation);
  }
  EXPECT_FALSE(cpuReservations.empty());
  EXPECT_TRUE(cpuBudget.rejected());
  EXPECT_EQ(cpuBudget.captureBytesReserved(), cpuReservations.size() * kCaptureBytes);
  EXPECT_EQ(cpuBudget.retainedBytes(), cpuReservations.size() * kCaptureBytes);
  EXPECT_LE(cpuBudget.retainedBytes(), components::kMaximumFilterFrameBytes);
  for (auto& reservation : cpuReservations) {
    cpuBudget.release(reservation);
  }
  EXPECT_EQ(cpuBudget.retainedBytes(), 0);

  components::FilterExecutionBudget gpuBudget;
  std::size_t gpuReservations = 0;
  while (gpuBudget.reserve(graph, kPixels, components::FilterMemoryModel::GpuAllNodes,
                           kCaptureBytes)) {
    ++gpuReservations;
  }
  EXPECT_GT(gpuReservations, 0u);
  EXPECT_TRUE(gpuBudget.rejected());
  EXPECT_EQ(gpuBudget.captureBytesReserved(), gpuReservations * kCaptureBytes);
  EXPECT_LE(gpuBudget.retainedBytes(), components::kMaximumFilterFrameBytes);
}

TEST(FilterGraphExecutorTest, AccountsForMergeFanInWorkAndMemory) {
  components::FilterGraph graph;
  components::FilterNode merge;
  merge.primitive = components::filter_primitive::Merge{};
  merge.inputs.resize(components::kMaximumFilterMergeInputs,
                      components::FilterInput{components::FilterInput::Previous{}});
  graph.nodes.push_back(std::move(merge));

  EXPECT_FALSE(components::FilterGraphFitsExecutionBudget(
      graph, 2049ULL * 1024, components::FilterMemoryModel::CpuFloatNamedResults));

  graph.nodes[0].inputs.push_back(components::FilterInput{components::FilterInput::Previous{}});
  EXPECT_FALSE(components::FilterGraphFitsExecutionBudget(
      graph, 1, components::FilterMemoryModel::CpuFloatNamedResults));
  EXPECT_FALSE(components::FilterGraphFitsExecutionBudget(
      graph, 1, components::FilterMemoryModel::GpuAllNodes));
}

TEST(FilterGraphExecutorTest, InvalidConvolveDivisorFailsClosed) {
  auto maybePixmap = tiny_skia::Pixmap::fromSize(2, 2);
  ASSERT_TRUE(maybePixmap.has_value());
  tiny_skia::Pixmap pixmap = std::move(*maybePixmap);

  components::FilterGraph graph;
  components::FilterNode node;
  node.primitive = components::filter_primitive::ConvolveMatrix{
      .orderX = 1,
      .orderY = 1,
      .kernelMatrix = {1.0},
      .divisor = 0.0,
  };
  graph.nodes.push_back(std::move(node));
  ApplyFilterGraphToPixmap(pixmap, graph, Transform2d(), std::nullopt);
  EXPECT_EQ(pixmap.width(), 2u);
  EXPECT_EQ(pixmap.height(), 2u);
}

// Regression: a filter region whose origin x maps past the pixmap width writes out of bounds.
// In the axis-aligned clip, x0 = max(0, floor(originX)) was clamped at the low end but NOT the high
// end, while x1 = clamp(ceil(right), 0, width) was clamped to width. With originX = 15 on a 10-wide
// pixmap, x0 = 15 and x1 = 10, so the "clear the left border [0, x0)" fill writes x0*4 = 60 bytes
// into each 40-byte row; on the last row that runs 20 bytes past the 400-byte buffer (heap-buffer-
// overflow under ASan; corrupted pixels otherwise). The region is fully outside, so every pixel
// must be cleared and no write may leave the buffer.
TEST(FilterGraphExecutorTest, ClipsRegionWithOriginPastPixmapWidth) {
  auto maybePixmap = tiny_skia::Pixmap::fromSize(10, 10);
  ASSERT_TRUE(maybePixmap.has_value());
  tiny_skia::Pixmap pixmap = std::move(*maybePixmap);

  for (int y = 0; y < 10; ++y) {
    for (int x = 0; x < 10; ++x) {
      SetPixel(pixmap, x, y, Pixel{255, 0, 0, 255});
    }
  }

  // Origin x = 15, right edge x = 20, both past the 10-px width; covers the full height.
  ClipFilterOutputToRegion(pixmap, Box2d::FromXYWH(15.0, 0.0, 5.0, 10.0), Transform2d());

  for (int y = 0; y < 10; ++y) {
    for (int x = 0; x < 10; ++x) {
      EXPECT_THAT(GetPixel(pixmap, x, y), Rgba(0, 0, 0, 0)) << "at " << x << "," << y;
    }
  }
}

// Regression: a filter region whose origin y maps past the pixmap height walks the first clear loop
// (for y in [0, y0)) past the buffer. y0 = max(0, floor(originY)) was not clamped to height, so the
// fill at data + y*width*4 indexes out of bounds for y >= height. The region is fully outside, so
// every pixel must be cleared and no write may leave the buffer.
TEST(FilterGraphExecutorTest, ClipsRegionWithOriginPastPixmapHeight) {
  auto maybePixmap = tiny_skia::Pixmap::fromSize(10, 10);
  ASSERT_TRUE(maybePixmap.has_value());
  tiny_skia::Pixmap pixmap = std::move(*maybePixmap);

  for (int y = 0; y < 10; ++y) {
    for (int x = 0; x < 10; ++x) {
      SetPixel(pixmap, x, y, Pixel{0, 255, 0, 255});
    }
  }

  // Origin y = 15, bottom edge y = 20, both past the 10-px height; covers the full width.
  ClipFilterOutputToRegion(pixmap, Box2d::FromXYWH(0.0, 15.0, 10.0, 5.0), Transform2d());

  for (int y = 0; y < 10; ++y) {
    for (int x = 0; x < 10; ++x) {
      EXPECT_THAT(GetPixel(pixmap, x, y), Rgba(0, 0, 0, 0)) << "at " << x << "," << y;
    }
  }
}

// Regression: a filter region whose origin is in-bounds but whose far edge runs past the surface
// must keep the in-bounds portion and clear the rest, with no out-of-bounds write from the
// overlarge far edge. Keep [4,10) x [4,10) on a 10x10 pixmap; right/bottom extend well past the
// surface.
TEST(FilterGraphExecutorTest, ClipsRegionPartlyPastPixmapKeepsInBounds) {
  auto maybePixmap = tiny_skia::Pixmap::fromSize(10, 10);
  ASSERT_TRUE(maybePixmap.has_value());
  tiny_skia::Pixmap pixmap = std::move(*maybePixmap);

  for (int y = 0; y < 10; ++y) {
    for (int x = 0; x < 10; ++x) {
      SetPixel(pixmap, x, y, Pixel{255, 255, 255, 255});
    }
  }

  ClipFilterOutputToRegion(pixmap, Box2d::FromXYWH(4.0, 4.0, 100.0, 100.0), Transform2d());

  for (int y = 0; y < 10; ++y) {
    for (int x = 0; x < 10; ++x) {
      const bool inside = (x >= 4 && y >= 4);
      EXPECT_THAT(GetPixel(pixmap, x, y), inside ? Rgba(255, 255, 255, 255) : Rgba(0, 0, 0, 0))
          << "at " << x << "," << y;
    }
  }
}

// Regression for applySubregionClipping (the vendored tiny-skia filter rasterizer) reached through
// ApplyFilterGraphToPixmap: a primitive subregion whose origin maps past the pixmap is clamped at
// the low end (max(0, ...)) but not the high end, so the per-row border fill writes out of bounds.
// Drive it via a Flood node with x/y placed past the surface; the flood must produce no opaque
// pixel inside the pixmap and must not write out of bounds.
TEST(FilterGraphExecutorTest, SubregionClipWithOriginPastPixmapDoesNotOverflow) {
  auto maybePixmap = tiny_skia::Pixmap::fromSize(10, 10);
  ASSERT_TRUE(maybePixmap.has_value());
  tiny_skia::Pixmap pixmap = std::move(*maybePixmap);

  components::FilterGraph graph;
  components::FilterNode node;
  node.inputs.push_back(components::FilterInput{});
  node.primitive = components::filter_primitive::Flood{
      .floodColor = css::Color(css::RGBA(0, 0, 255, 255)),
      .floodOpacity = 1.0,
  };
  // Subregion origin (15,0) is past the 10-px width; nothing should land inside the pixmap.
  node.x = Lengthd(15.0);
  node.y = Lengthd(0.0);
  node.width = Lengthd(5.0);
  node.height = Lengthd(10.0);
  graph.nodes.push_back(std::move(node));

  ApplyFilterGraphToPixmap(pixmap, graph, Transform2d(), Box2d::FromXYWH(0.0, 0.0, 10.0, 10.0));

  for (int y = 0; y < 10; ++y) {
    for (int x = 0; x < 10; ++x) {
      EXPECT_THAT(GetPixel(pixmap, x, y), Rgba(0, 0, 0, 0)) << "at " << x << "," << y;
    }
  }
}

TEST(FilterGraphExecutorTest, RotationDoesNotChangeBlurSigma) {
  const tiny_skia::Pixmap identityBlurred = CreateBlurredDotPixmap(Transform2d());
  const tiny_skia::Pixmap rotatedBlurred = CreateBlurredDotPixmap(Transform2d::Rotate(M_PI / 4.0));

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

  ApplyFilterGraphToPixmap(pixmap, graph, Transform2d(), Box2d::FromXYWH(4.0, 4.0, 16.0, 16.0));

  EXPECT_THAT(GetPixel(pixmap, 7, 7), Rgba(0, 0, 0, 0));
  EXPECT_THAT(GetPixel(pixmap, 8, 8), Rgba(255, 0, 0, 255));
  EXPECT_THAT(GetPixel(pixmap, 19, 19), Rgba(255, 0, 0, 255));
  EXPECT_THAT(GetPixel(pixmap, 20, 20), Rgba(0, 0, 0, 0));
}

TEST(FilterGraphExecutorTest, PrimitiveSubregionPercentagesUseElementBoundsInObjectBoundingBox) {
  auto maybePixmap = tiny_skia::Pixmap::fromSize(32, 32);
  ASSERT_TRUE(maybePixmap.has_value());
  tiny_skia::Pixmap pixmap = std::move(*maybePixmap);

  components::FilterGraph graph;
  graph.primitiveUnits = PrimitiveUnits::ObjectBoundingBox;
  graph.elementBoundingBox = Box2d::FromXYWH(10.0, 10.0, 20.0, 20.0);

  components::FilterNode node;
  node.inputs.push_back(components::FilterInput{});
  node.primitive = components::filter_primitive::Flood{
      .floodColor = css::Color(css::RGBA(0, 255, 0, 255)),
      .floodOpacity = 1.0,
  };
  node.width = Lengthd(50.0, Lengthd::Unit::Percent);
  node.height = Lengthd(50.0, Lengthd::Unit::Percent);
  graph.nodes.push_back(std::move(node));

  ApplyFilterGraphToPixmap(pixmap, graph, Transform2d(), std::nullopt);

  EXPECT_THAT(GetPixel(pixmap, 9, 9), Rgba(0, 0, 0, 0));
  EXPECT_THAT(GetPixel(pixmap, 10, 10), Rgba(0, 255, 0, 255));
  EXPECT_THAT(GetPixel(pixmap, 19, 19), Rgba(0, 255, 0, 255));
  EXPECT_THAT(GetPixel(pixmap, 20, 20), Rgba(0, 0, 0, 0));
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

  ApplyFilterGraphToPixmap(pixmap, graph, Transform2d(), Box2d::FromXYWH(0.0, 0.0, 32.0, 32.0));

  // Flood at (16,16) size 1x1, blur σ=2 → subregion expands by ceil(2*3)=6.
  // Expanded subregion: x=[10,23], y=[10,23]. Pixel (9,16) is outside.
  EXPECT_THAT(GetPixel(pixmap, 9, 16), Rgba(0, 0, 0, 0));
  EXPECT_THAT(GetPixel(pixmap, 15, 16), Rgba(_, _, _, Gt(0)));
  EXPECT_THAT(GetPixel(pixmap, 16, 15), Rgba(_, _, _, Gt(0)));
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

  ApplyFilterGraphToPixmap(pixmap, graph, Transform2d(), Box2d::FromXYWH(0.0, 0.0, 8.0, 8.0), true);

  EXPECT_THAT(GetPixel(pixmap, 7, 4), Rgba(_, _, _, AllOf(Gt(0), Lt(255))));
  EXPECT_THAT(GetPixel(pixmap, 12, 4), Rgba(0, 0, 0, 0));
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

  ApplyFilterGraphToPixmap(pixmap, graph, Transform2d(), std::nullopt, false, &fillPaintPixmap);

  EXPECT_THAT(GetPixel(pixmap, 1, 1), Rgba(0, 0, 0, 0));
  EXPECT_THAT(GetPixel(pixmap, 2, 2), Rgba(0, 255, 0, 255));
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

  ApplyFilterGraphToPixmap(pixmap, graph, Transform2d(), std::nullopt);

  EXPECT_THAT(GetPixel(pixmap, 1, 1), Rgba(0, 0, 0, 0));
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

  ApplyFilterGraphToPixmap(pixmap, graph, Transform2d(), Box2d::FromXYWH(0.0, 0.0, 32.0, 32.0));

  // After offset by 4, columns 0-3 should be transparent. Column 0 should remain zero even after
  // the small blur because sigma=1 doesn't spread 4 pixels.
  EXPECT_THAT(GetPixel(pixmap, 0, 16), Rgba(0, 0, 0, 0));
  // Deep inside the offset region, the pixel should still be red (possibly slightly blurred at
  // edges, but center stays opaque).
  const Pixel center = GetPixel(pixmap, 20, 16);
  EXPECT_THAT(center, Rgba(255, _, _, 255));
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

  ApplyFilterGraphToPixmap(pixmap, graph, Transform2d(), Box2d::FromXYWH(0.0, 0.0, 16.0, 16.0));

  // The green source pixel at (8,8) should still be present.
  const Pixel greenPixel = GetPixel(pixmap, 8, 8);
  EXPECT_THAT(greenPixel, Rgba(_, Gt(0), _, 255));

  // The flood was at (4,4)-(8,8) and was offset by (2,2), so the blue region is at (6,6)-(10,10).
  // Check a pixel inside the flood region that isn't (8,8).
  const Pixel bluePixel = GetPixel(pixmap, 7, 7);
  EXPECT_THAT(bluePixel, Rgba(_, _, Gt(0), 255));
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

  // Node 2: Identity offset referencing "redFlood" by name - should produce red, not green.
  components::FilterNode offsetNode;
  offsetNode.inputs.push_back(
      components::FilterInput{components::FilterInput::Named{RcString("redFlood")}});
  offsetNode.primitive = components::filter_primitive::Offset{.dx = 0.0, .dy = 0.0};
  graph.nodes.push_back(std::move(offsetNode));

  ApplyFilterGraphToPixmap(pixmap, graph, Transform2d(), std::nullopt);

  // The final output should be the red flood (from the named buffer), not the green one.
  EXPECT_THAT(GetPixel(pixmap, 4, 4), Rgba(255, 0, 0, 255));
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

  // Node 1: Merge the named buffer with itself - both inputs reference "blueFlood".
  components::FilterNode mergeNode;
  mergeNode.inputs.push_back(
      components::FilterInput{components::FilterInput::Named{RcString("blueFlood")}});
  mergeNode.inputs.push_back(
      components::FilterInput{components::FilterInput::Named{RcString("blueFlood")}});
  mergeNode.primitive = components::filter_primitive::Merge{};
  graph.nodes.push_back(std::move(mergeNode));

  ApplyFilterGraphToPixmap(pixmap, graph, Transform2d(), std::nullopt);

  // Output should be blue (merged blue over blue = blue).
  EXPECT_THAT(GetPixel(pixmap, 8, 8), Rgba(0, 0, 255, 255));
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

  ApplyFilterGraphToPixmap(pixmap, graph, Transform2d(), Box2d::FromXYWH(0.0, 0.0, 32.0, 32.0));

  // The original white dot should still be present.
  const Pixel orig = GetPixel(pixmap, 16, 16);
  EXPECT_THAT(orig, Rgba(255, _, _, 255));

  // The shadow center should be near (20, 20), with non-zero alpha (black shadow).
  const Pixel shadow = GetPixel(pixmap, 20, 20);
  EXPECT_THAT(shadow, Rgba(_, _, _, Gt(0)));
}

// ---------------------------------------------------------------------------
// feDropShadow input selection
// ---------------------------------------------------------------------------

/// Fills a square of opaque white, the simplest shape whose alpha is either 255 or 0 everywhere
/// so a shadow cast from it can be located exactly.
void FillOpaqueSquare(tiny_skia::Pixmap& pixmap, int x, int y, int size) {
  for (int row = y; row < y + size; ++row) {
    for (int column = x; column < x + size; ++column) {
      SetPixel(pixmap, column, row, Pixel{255, 255, 255, 255});
    }
  }
}

/// A drop shadow with no blur, so the shadow is a hard-edged copy of its input's alpha and its
/// position can be asserted per-pixel instead of through a tolerance.
components::filter_primitive::DropShadow SharpBlackShadow(double dx, double dy) {
  return components::filter_primitive::DropShadow{
      .dx = dx,
      .dy = dy,
      .stdDeviationX = 0.0,
      .stdDeviationY = 0.0,
      .floodColor = css::Color(css::RGBA(0, 0, 0, 255)),
      .floodOpacity = 1.0,
  };
}

// feDropShadow builds its shadow from the alpha of its own input, and an explicit `in` naming
// an earlier result has to move the shadow with that result. A shadow cut from SourceGraphic
// instead stays behind at the unfiltered geometry.
TEST(FilterGraphExecutorTest, DropShadowUsesNamedInputAlphaNotSourceGraphic) {
  auto maybePixmap = tiny_skia::Pixmap::fromSize(32, 32);
  ASSERT_TRUE(maybePixmap.has_value());
  tiny_skia::Pixmap pixmap = std::move(*maybePixmap);

  // SourceGraphic is one opaque square covering x,y in [6, 10).
  FillOpaqueSquare(pixmap, 6, 6, 4);

  components::FilterGraph graph;

  // Node 0: move the square to [18, 22) and publish it as "moved".
  components::FilterNode offsetNode;
  offsetNode.inputs.push_back(
      components::FilterInput{components::FilterStandardInput::SourceGraphic});
  offsetNode.primitive = components::filter_primitive::Offset{.dx = 12.0, .dy = 12.0};
  offsetNode.result = RcString("moved");
  graph.nodes.push_back(std::move(offsetNode));

  // Node 1: shadow "moved", putting the shadow at [22, 26).
  components::FilterNode shadowNode;
  shadowNode.inputs.push_back(
      components::FilterInput{components::FilterInput::Named{RcString("moved")}});
  shadowNode.primitive = SharpBlackShadow(/*dx=*/4.0, /*dy=*/4.0);
  graph.nodes.push_back(std::move(shadowNode));

  ApplyFilterGraphToPixmap(pixmap, graph, Transform2d(), std::nullopt);

  // The input the shadow was cut from is still merged over it.
  EXPECT_THAT(GetPixel(pixmap, 19, 19), Rgba(255, 255, 255, 255));

  // The shadow follows the named input.
  EXPECT_THAT(GetPixel(pixmap, 23, 23), Rgba(0, 0, 0, 255));

  // ... and no shadow is cast from SourceGraphic's untouched position, which is where a shadow
  // built from SourceGraphic's alpha would have landed.
  EXPECT_THAT(GetPixel(pixmap, 11, 11), Rgba(_, _, _, 0));
}

// The same obligation reached through the implicit input: an unspecified `in` is the preceding
// primitive's result, so only a feDropShadow that is itself the first primitive resolves to
// SourceGraphic.
TEST(FilterGraphExecutorTest, DropShadowUsesPrecedingResultAlphaWhenInputIsImplicit) {
  auto maybePixmap = tiny_skia::Pixmap::fromSize(32, 32);
  ASSERT_TRUE(maybePixmap.has_value());
  tiny_skia::Pixmap pixmap = std::move(*maybePixmap);

  FillOpaqueSquare(pixmap, 6, 6, 4);

  components::FilterGraph graph;

  components::FilterNode offsetNode;
  offsetNode.inputs.push_back(
      components::FilterInput{components::FilterStandardInput::SourceGraphic});
  offsetNode.primitive = components::filter_primitive::Offset{.dx = 12.0, .dy = 12.0};
  graph.nodes.push_back(std::move(offsetNode));

  components::FilterNode shadowNode;
  shadowNode.inputs.push_back(components::FilterInput{});  // Previous.
  shadowNode.primitive = SharpBlackShadow(/*dx=*/4.0, /*dy=*/4.0);
  graph.nodes.push_back(std::move(shadowNode));

  ApplyFilterGraphToPixmap(pixmap, graph, Transform2d(), std::nullopt);

  EXPECT_THAT(GetPixel(pixmap, 19, 19), Rgba(255, 255, 255, 255));
  EXPECT_THAT(GetPixel(pixmap, 23, 23), Rgba(0, 0, 0, 255));
  EXPECT_THAT(GetPixel(pixmap, 11, 11), Rgba(_, _, _, 0));
}

// The default case, which the two above must not disturb: a feDropShadow with no earlier
// primitive to chain from still cuts its shadow out of SourceGraphic.
TEST(FilterGraphExecutorTest, DropShadowWithoutAnEarlierResultShadowsSourceGraphic) {
  auto maybePixmap = tiny_skia::Pixmap::fromSize(32, 32);
  ASSERT_TRUE(maybePixmap.has_value());
  tiny_skia::Pixmap pixmap = std::move(*maybePixmap);

  FillOpaqueSquare(pixmap, 6, 6, 4);

  components::FilterGraph graph;
  components::FilterNode shadowNode;
  shadowNode.inputs.push_back(components::FilterInput{});  // Previous, which is SourceGraphic here.
  shadowNode.primitive = SharpBlackShadow(/*dx=*/4.0, /*dy=*/4.0);
  graph.nodes.push_back(std::move(shadowNode));

  ApplyFilterGraphToPixmap(pixmap, graph, Transform2d(), std::nullopt);

  EXPECT_THAT(GetPixel(pixmap, 7, 7), Rgba(255, 255, 255, 255));
  EXPECT_THAT(GetPixel(pixmap, 11, 11), Rgba(0, 0, 0, 255));
}

// On a first primitive, an explicit `in="SourceGraphic"` and an implicit input name the same
// buffer, so the two renders have to agree byte for byte. This is the pixel-level statement that
// reading the node's own input left that path alone.
TEST(FilterGraphExecutorTest, DropShadowExplicitSourceGraphicMatchesImplicitInput) {
  const auto render = [](components::FilterInput input) {
    auto maybePixmap = tiny_skia::Pixmap::fromSize(32, 32);
    EXPECT_TRUE(maybePixmap.has_value());
    tiny_skia::Pixmap pixmap = std::move(*maybePixmap);

    // Partial alpha and mixed colors, so a shadow that reads the wrong channel or skips a
    // conversion shows up rather than cancelling against a flat opaque square.
    for (int y = 4; y < 12; ++y) {
      for (int x = 4; x < 12; ++x) {
        const auto alpha = static_cast<std::uint8_t>(40 + (x * 9 + y * 3) % 216);
        const auto scale = [&](int value) {
          return static_cast<std::uint8_t>(value * alpha / 255);
        };
        SetPixel(pixmap, x, y, Pixel{scale(200), scale(80), scale(30), alpha});
      }
    }

    components::FilterGraph graph;
    components::FilterNode shadowNode;
    shadowNode.inputs.push_back(std::move(input));
    shadowNode.primitive = components::filter_primitive::DropShadow{
        .dx = 3.0,
        .dy = 2.0,
        .stdDeviationX = 1.5,
        .stdDeviationY = 1.5,
        .floodColor = css::Color(css::RGBA(20, 90, 160, 255)),
        .floodOpacity = 0.75,
    };
    graph.nodes.push_back(std::move(shadowNode));

    ApplyFilterGraphToPixmap(pixmap, graph, Transform2d(), std::nullopt);
    return pixmap;
  };

  const tiny_skia::Pixmap implicitInput = render(components::FilterInput{});
  const tiny_skia::Pixmap explicitSourceGraphic =
      render(components::FilterInput{components::FilterStandardInput::SourceGraphic});

  const auto implicitData = implicitInput.data();
  const auto explicitData = explicitSourceGraphic.data();
  ASSERT_EQ(implicitData.size(), explicitData.size());
  EXPECT_THAT(std::vector<std::uint8_t>(explicitData.begin(), explicitData.end()),
              ElementsAreArray(implicitData));

  // The shadow is present, so the comparison above is not two blank pixmaps agreeing.
  EXPECT_THAT(GetPixel(implicitInput, 13, 12), Rgba(_, _, _, Gt(0)));
}

// ---------------------------------------------------------------------------
// Two-input primitive `in2` resolution
// ---------------------------------------------------------------------------

/// Fills a block of mixed colors and partial alpha, so a two-input primitive that reads the wrong
/// buffer or skips a color-space conversion shows up rather than cancelling against a flat opaque
/// square.
void FillMixedAlphaBlock(tiny_skia::Pixmap& pixmap) {
  for (int y = 4; y < 12; ++y) {
    for (int x = 4; x < 12; ++x) {
      const auto alpha = static_cast<std::uint8_t>(40 + (x * 9 + y * 3) % 216);
      const auto scale = [&](int value) { return static_cast<std::uint8_t>(value * alpha / 255); };
      SetPixel(pixmap, x, y, Pixel{scale(200), scale(80), scale(30), alpha});
    }
  }
}

/// Asserts two renders agree byte for byte. Used by the first-primitive cases, where an
/// unspecified `in2` and an explicit `in2="SourceGraphic"` name the same buffer.
void ExpectPixmapsEqual(const tiny_skia::Pixmap& lhs, const tiny_skia::Pixmap& rhs) {
  const auto lhsData = lhs.data();
  const auto rhsData = rhs.data();
  ASSERT_EQ(lhsData.size(), rhsData.size());
  EXPECT_THAT(std::vector<std::uint8_t>(rhsData.begin(), rhsData.end()), ElementsAreArray(lhsData));
}

/// A 16x16 pixmap holding one opaque white square covering x,y in [2, 10).
tiny_skia::Pixmap MakeSquarePixmap() {
  auto maybePixmap = tiny_skia::Pixmap::fromSize(16, 16);
  EXPECT_TRUE(maybePixmap.has_value());
  tiny_skia::Pixmap pixmap = std::move(*maybePixmap);
  FillOpaqueSquare(pixmap, 2, 2, 8);
  return pixmap;
}

/// An feOffset that shifts SourceGraphic right by six pixels, so the moved copy overlaps the
/// original only in x = [8, 10). A second input taken from the preceding result and one taken
/// from SourceGraphic therefore mask different pixels.
components::FilterNode MakeMovedSquareNode() {
  components::FilterNode node;
  node.inputs.push_back(components::FilterInput{components::FilterStandardInput::SourceGraphic});
  node.primitive = components::filter_primitive::Offset{.dx = 6.0, .dy = 0.0};
  return node;
}

// An unspecified `in2` resolves the same way an unspecified `in` does: to the preceding
// primitive's result, and to SourceGraphic only when the primitive is first in the filter. The
// mask here is therefore the moved square, not the unfiltered element the filter started from.
TEST(FilterGraphExecutorTest, CompositeImplicitIn2UsesPrecedingResult) {
  tiny_skia::Pixmap pixmap = MakeSquarePixmap();

  components::FilterGraph graph;
  graph.nodes.push_back(MakeMovedSquareNode());

  components::FilterNode compositeNode;
  compositeNode.inputs.push_back(
      components::FilterInput{components::FilterStandardInput::SourceGraphic});
  // No second input at all, which is how an unspecified `in2` reaches the executor.
  compositeNode.primitive = components::filter_primitive::Composite{
      .op = components::filter_primitive::Composite::Operator::In,
  };
  graph.nodes.push_back(std::move(compositeNode));

  ApplyFilterGraphToPixmap(pixmap, graph, Transform2d(), std::nullopt);

  // The overlap of the source square with the moved square survives.
  EXPECT_THAT(GetPixel(pixmap, 9, 4), Rgba(255, 255, 255, 255));

  // The rest of the source square is masked away. Masking against SourceGraphic would have kept
  // it, because a square is fully inside itself.
  EXPECT_THAT(GetPixel(pixmap, 3, 4), Rgba(_, _, _, 0));

  // `operator="in"` keeps nothing outside the first input either way.
  EXPECT_THAT(GetPixel(pixmap, 12, 4), Rgba(_, _, _, 0));
}

// An explicit `in2` still names its own buffer. Pointing a chained feComposite back at
// SourceGraphic keeps the whole source square, which is exactly what the implicit case must not
// do, so the two tests together pin the default rather than a hardcoded choice.
TEST(FilterGraphExecutorTest, CompositeExplicitIn2NamesItsOwnInput) {
  tiny_skia::Pixmap pixmap = MakeSquarePixmap();

  components::FilterGraph graph;
  graph.nodes.push_back(MakeMovedSquareNode());

  components::FilterNode compositeNode;
  compositeNode.inputs.push_back(
      components::FilterInput{components::FilterStandardInput::SourceGraphic});
  compositeNode.inputs.push_back(
      components::FilterInput{components::FilterStandardInput::SourceGraphic});
  compositeNode.primitive = components::filter_primitive::Composite{
      .op = components::filter_primitive::Composite::Operator::In,
  };
  graph.nodes.push_back(std::move(compositeNode));

  ApplyFilterGraphToPixmap(pixmap, graph, Transform2d(), std::nullopt);

  EXPECT_THAT(GetPixel(pixmap, 3, 4), Rgba(255, 255, 255, 255));
  EXPECT_THAT(GetPixel(pixmap, 9, 4), Rgba(255, 255, 255, 255));
  EXPECT_THAT(GetPixel(pixmap, 12, 4), Rgba(_, _, _, 0));
}

// On a first primitive there is no preceding result, so an unspecified `in2` and an explicit
// `in2="SourceGraphic"` name the same buffer and the two renders have to agree byte for byte.
// This is the pixel-level statement that single-primitive filters are unaffected.
TEST(FilterGraphExecutorTest, CompositeFirstPrimitiveImplicitIn2MatchesSourceGraphic) {
  const auto render = [](bool explicitIn2) {
    auto maybePixmap = tiny_skia::Pixmap::fromSize(16, 16);
    EXPECT_TRUE(maybePixmap.has_value());
    tiny_skia::Pixmap pixmap = std::move(*maybePixmap);
    FillMixedAlphaBlock(pixmap);

    components::FilterGraph graph;
    components::FilterNode node;
    node.inputs.push_back(components::FilterInput{components::FilterStandardInput::SourceGraphic});
    if (explicitIn2) {
      node.inputs.push_back(
          components::FilterInput{components::FilterStandardInput::SourceGraphic});
    }
    // Arithmetic mixes both inputs channel by channel, so a wrong second buffer cannot hide
    // behind a Porter-Duff operator that happens to ignore it.
    node.primitive = components::filter_primitive::Composite{
        .op = components::filter_primitive::Composite::Operator::Arithmetic,
        .k1 = 0.5,
        .k2 = 0.25,
        .k3 = 0.25,
        .k4 = 0.0,
    };
    graph.nodes.push_back(std::move(node));

    ApplyFilterGraphToPixmap(pixmap, graph, Transform2d(), std::nullopt);
    return pixmap;
  };

  const tiny_skia::Pixmap implicitIn2 = render(false);
  const tiny_skia::Pixmap explicitIn2 = render(true);
  ExpectPixmapsEqual(implicitIn2, explicitIn2);

  // The composite produced something, so the comparison is not two blank pixmaps agreeing.
  EXPECT_THAT(GetPixel(implicitIn2, 8, 8), Rgba(_, _, _, Gt(0)));
}

// feBlend's `in2` is the backdrop, and it defaults the same way: the preceding flood, not the
// white source graphic.
TEST(FilterGraphExecutorTest, BlendImplicitIn2UsesPrecedingResult) {
  auto maybePixmap = tiny_skia::Pixmap::fromSize(16, 16);
  ASSERT_TRUE(maybePixmap.has_value());
  tiny_skia::Pixmap pixmap = std::move(*maybePixmap);
  FillOpaqueSquare(pixmap, 0, 0, 16);

  components::FilterGraph graph;
  // sRGB keeps `multiply` a plain per-channel product of the authored values.
  graph.colorInterpolationFilters = ColorInterpolationFilters::SRGB;

  components::FilterNode floodNode;
  floodNode.inputs.push_back(components::FilterInput{});
  floodNode.primitive = components::filter_primitive::Flood{
      .floodColor = css::Color(css::RGBA(128, 128, 128, 255)),
      .floodOpacity = 1.0,
  };
  graph.nodes.push_back(std::move(floodNode));

  components::FilterNode blendNode;
  blendNode.inputs.push_back(
      components::FilterInput{components::FilterStandardInput::SourceGraphic});
  // No second input at all, which is how an unspecified `in2` reaches the executor.
  blendNode.primitive = components::filter_primitive::Blend{
      .mode = components::filter_primitive::Blend::Mode::Multiply,
  };
  graph.nodes.push_back(std::move(blendNode));

  ApplyFilterGraphToPixmap(pixmap, graph, Transform2d(), std::nullopt);

  // White multiplied by the 50% gray flood. Blending against SourceGraphic would have multiplied
  // white by white and left the pixmap at 255.
  EXPECT_THAT(GetPixel(pixmap, 8, 8),
              Rgba(ChannelNear(128, 2), ChannelNear(128, 2), ChannelNear(128, 2), 255));
}

// The explicit counterpart: naming SourceGraphic as the backdrop multiplies white by white and
// leaves the pixmap untouched, so the default above is a default and not a rewrite.
TEST(FilterGraphExecutorTest, BlendExplicitIn2NamesItsOwnInput) {
  auto maybePixmap = tiny_skia::Pixmap::fromSize(16, 16);
  ASSERT_TRUE(maybePixmap.has_value());
  tiny_skia::Pixmap pixmap = std::move(*maybePixmap);
  FillOpaqueSquare(pixmap, 0, 0, 16);

  components::FilterGraph graph;
  graph.colorInterpolationFilters = ColorInterpolationFilters::SRGB;

  components::FilterNode floodNode;
  floodNode.inputs.push_back(components::FilterInput{});
  floodNode.primitive = components::filter_primitive::Flood{
      .floodColor = css::Color(css::RGBA(128, 128, 128, 255)),
      .floodOpacity = 1.0,
  };
  graph.nodes.push_back(std::move(floodNode));

  components::FilterNode blendNode;
  blendNode.inputs.push_back(
      components::FilterInput{components::FilterStandardInput::SourceGraphic});
  blendNode.inputs.push_back(
      components::FilterInput{components::FilterStandardInput::SourceGraphic});
  blendNode.primitive = components::filter_primitive::Blend{
      .mode = components::filter_primitive::Blend::Mode::Multiply,
  };
  graph.nodes.push_back(std::move(blendNode));

  ApplyFilterGraphToPixmap(pixmap, graph, Transform2d(), std::nullopt);

  EXPECT_THAT(GetPixel(pixmap, 8, 8), Rgba(255, 255, 255, 255));
}

// The first-primitive identity for feBlend: with nothing before it, the implicit backdrop is
// SourceGraphic, so naming it explicitly has to render the same bytes.
TEST(FilterGraphExecutorTest, BlendFirstPrimitiveImplicitIn2MatchesSourceGraphic) {
  const auto render = [](bool explicitIn2) {
    auto maybePixmap = tiny_skia::Pixmap::fromSize(16, 16);
    EXPECT_TRUE(maybePixmap.has_value());
    tiny_skia::Pixmap pixmap = std::move(*maybePixmap);
    FillMixedAlphaBlock(pixmap);

    components::FilterGraph graph;
    components::FilterNode node;
    node.inputs.push_back(components::FilterInput{components::FilterStandardInput::SourceGraphic});
    if (explicitIn2) {
      node.inputs.push_back(
          components::FilterInput{components::FilterStandardInput::SourceGraphic});
    }
    node.primitive = components::filter_primitive::Blend{
        .mode = components::filter_primitive::Blend::Mode::Multiply,
    };
    graph.nodes.push_back(std::move(node));

    ApplyFilterGraphToPixmap(pixmap, graph, Transform2d(), std::nullopt);
    return pixmap;
  };

  const tiny_skia::Pixmap implicitIn2 = render(false);
  const tiny_skia::Pixmap explicitIn2 = render(true);
  ExpectPixmapsEqual(implicitIn2, explicitIn2);

  EXPECT_THAT(GetPixel(implicitIn2, 8, 8), Rgba(_, _, _, Gt(0)));
}

// feDisplacementMap's `in2` is the displacement map itself, so the default decides where every
// output pixel is sampled from. An opaque flood displaces the whole image by a constant; the
// source graphic's own alpha would displace the square and its surroundings in opposite
// directions.
TEST(FilterGraphExecutorTest, DisplacementMapImplicitIn2UsesPrecedingResult) {
  auto maybePixmap = tiny_skia::Pixmap::fromSize(16, 16);
  ASSERT_TRUE(maybePixmap.has_value());
  tiny_skia::Pixmap pixmap = std::move(*maybePixmap);
  FillOpaqueSquare(pixmap, 4, 4, 4);

  components::FilterGraph graph;

  components::FilterNode floodNode;
  floodNode.inputs.push_back(components::FilterInput{});
  floodNode.primitive = components::filter_primitive::Flood{
      .floodColor = css::Color(css::RGBA(255, 255, 255, 255)),
      .floodOpacity = 1.0,
  };
  graph.nodes.push_back(std::move(floodNode));

  components::FilterNode displaceNode;
  displaceNode.inputs.push_back(
      components::FilterInput{components::FilterStandardInput::SourceGraphic});
  // No second input at all, which is how an unspecified `in2` reaches the executor.
  displaceNode.primitive = components::filter_primitive::DisplacementMap{
      .scale = 8.0,
      .xChannelSelector = components::filter_primitive::DisplacementMap::Channel::A,
      .yChannelSelector = components::filter_primitive::DisplacementMap::Channel::A,
  };
  graph.nodes.push_back(std::move(displaceNode));

  ApplyFilterGraphToPixmap(pixmap, graph, Transform2d(), std::nullopt);

  // Alpha is 1 everywhere in the flood, so every pixel samples 8 * (1 - 0.5) = 4 pixels down and
  // to the right: the square moves from [4, 8) to [0, 4).
  EXPECT_THAT(GetPixel(pixmap, 2, 2), Rgba(255, 255, 255, 255));

  // Displacing by SourceGraphic's own alpha instead would have sent the pixels outside the
  // square the other way and landed a copy of the square at [8, 12).
  EXPECT_THAT(GetPixel(pixmap, 10, 10), Rgba(_, _, _, 0));
}

// The explicit counterpart, which is also the pre-default behavior: naming SourceGraphic as the
// displacement map splits the image, because its alpha is 1 inside the square and 0 outside.
TEST(FilterGraphExecutorTest, DisplacementMapExplicitIn2NamesItsOwnInput) {
  auto maybePixmap = tiny_skia::Pixmap::fromSize(16, 16);
  ASSERT_TRUE(maybePixmap.has_value());
  tiny_skia::Pixmap pixmap = std::move(*maybePixmap);
  FillOpaqueSquare(pixmap, 4, 4, 4);

  components::FilterGraph graph;

  components::FilterNode floodNode;
  floodNode.inputs.push_back(components::FilterInput{});
  floodNode.primitive = components::filter_primitive::Flood{
      .floodColor = css::Color(css::RGBA(255, 255, 255, 255)),
      .floodOpacity = 1.0,
  };
  graph.nodes.push_back(std::move(floodNode));

  components::FilterNode displaceNode;
  displaceNode.inputs.push_back(
      components::FilterInput{components::FilterStandardInput::SourceGraphic});
  displaceNode.inputs.push_back(
      components::FilterInput{components::FilterStandardInput::SourceGraphic});
  displaceNode.primitive = components::filter_primitive::DisplacementMap{
      .scale = 8.0,
      .xChannelSelector = components::filter_primitive::DisplacementMap::Channel::A,
      .yChannelSelector = components::filter_primitive::DisplacementMap::Channel::A,
  };
  graph.nodes.push_back(std::move(displaceNode));

  ApplyFilterGraphToPixmap(pixmap, graph, Transform2d(), std::nullopt);

  // Alpha 0 outside the square sends those pixels 4 up and to the left, so [8, 12) samples the
  // square at [4, 8).
  EXPECT_THAT(GetPixel(pixmap, 10, 10), Rgba(255, 255, 255, 255));
  EXPECT_THAT(GetPixel(pixmap, 2, 2), Rgba(_, _, _, 0));
}

// The first-primitive identity for feDisplacementMap: the implicit map is SourceGraphic when
// nothing precedes the primitive, so both spellings render the same bytes.
TEST(FilterGraphExecutorTest, DisplacementMapFirstPrimitiveImplicitIn2MatchesSourceGraphic) {
  const auto render = [](bool explicitIn2) {
    auto maybePixmap = tiny_skia::Pixmap::fromSize(16, 16);
    EXPECT_TRUE(maybePixmap.has_value());
    tiny_skia::Pixmap pixmap = std::move(*maybePixmap);
    FillMixedAlphaBlock(pixmap);

    components::FilterGraph graph;
    components::FilterNode node;
    node.inputs.push_back(components::FilterInput{components::FilterStandardInput::SourceGraphic});
    if (explicitIn2) {
      node.inputs.push_back(
          components::FilterInput{components::FilterStandardInput::SourceGraphic});
    }
    node.primitive = components::filter_primitive::DisplacementMap{
        .scale = 6.0,
        .xChannelSelector = components::filter_primitive::DisplacementMap::Channel::R,
        .yChannelSelector = components::filter_primitive::DisplacementMap::Channel::A,
    };
    graph.nodes.push_back(std::move(node));

    ApplyFilterGraphToPixmap(pixmap, graph, Transform2d(), std::nullopt);
    return pixmap;
  };

  const tiny_skia::Pixmap implicitIn2 = render(false);
  const tiny_skia::Pixmap explicitIn2 = render(true);
  ExpectPixmapsEqual(implicitIn2, explicitIn2);

  EXPECT_THAT(GetPixel(implicitIn2, 6, 6), Rgba(_, _, _, Gt(0)));
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

  ApplyFilterGraphToPixmap(pixmap, graph, Transform2d(), std::nullopt);

  // The pixel should remain unchanged.
  EXPECT_THAT(GetPixel(pixmap, 3, 3), Rgba(42, 100, 200, 255));
}

TEST(FilterGraphExecutorTest, FilterWithNoSourceGraphicContent) {
  // SourceGraphic is entirely transparent - filter should still execute.
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

  ApplyFilterGraphToPixmap(pixmap, graph, Transform2d(), std::nullopt);

  // Even though SourceGraphic was transparent, the flood should fill the pixmap.
  EXPECT_THAT(GetPixel(pixmap, 4, 4), Rgba(128, 64, 32, 255));
}

TEST(FilterGraphExecutorTest, SourceAlphaInputExtractsAlphaChannel) {
  auto maybePixmap = tiny_skia::Pixmap::fromSize(8, 8);
  ASSERT_TRUE(maybePixmap.has_value());
  tiny_skia::Pixmap pixmap = std::move(*maybePixmap);

  // Set a colored pixel with full alpha.
  SetPixel(pixmap, 4, 4, Pixel{200, 100, 50, 255});

  components::FilterGraph graph;
  components::FilterNode node;
  // Use SourceAlpha as input - should zero out RGB, keep alpha.
  node.inputs.push_back(components::FilterInput{components::FilterStandardInput::SourceAlpha});
  node.primitive = components::filter_primitive::Offset{.dx = 0.0, .dy = 0.0};
  graph.nodes.push_back(std::move(node));

  ApplyFilterGraphToPixmap(pixmap, graph, Transform2d(), std::nullopt);

  const Pixel result = GetPixel(pixmap, 4, 4);
  // SourceAlpha produces (0, 0, 0, alpha).
  EXPECT_THAT(result, Rgba(0, 0, 0, 255));
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

    ApplyFilterGraphToPixmap(pixmap, graph, Transform2d(), std::nullopt);
    return pixmap;
  };

  const tiny_skia::Pixmap srgbResult = createBlendedPixmap(ColorInterpolationFilters::SRGB);
  const tiny_skia::Pixmap linearResult = createBlendedPixmap(ColorInterpolationFilters::LinearRGB);

  // Both should produce valid non-empty output. The actual pixel values may or may not differ
  // depending on the blend mode and input colors (Normal blend with premultiplied alpha can
  // produce identical results in both color spaces for some inputs).
  EXPECT_FALSE(srgbResult.data().empty());
  EXPECT_FALSE(linearResult.data().empty());
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
  ApplyFilterGraphToPixmap(pixmap, graph, Transform2d(), std::nullopt);

  // Center pixel should still be mid-gray (blur of uniform input is identity).
  const Pixel center = GetPixel(pixmap, 4, 4);
  EXPECT_THAT(center, Rgba(ChannelNear(128, 2), ChannelNear(128, 2), ChannelNear(128, 2), 255));
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

  // Node 1: Composite SourceGraphic IN greenRegion - only keep source pixels where green is opaque.
  components::FilterNode compositeNode;
  compositeNode.inputs.push_back(
      components::FilterInput{components::FilterStandardInput::SourceGraphic});
  compositeNode.inputs.push_back(
      components::FilterInput{components::FilterInput::Named{RcString("greenRegion")}});
  compositeNode.primitive = components::filter_primitive::Composite{
      .op = components::filter_primitive::Composite::Operator::In,
  };
  graph.nodes.push_back(std::move(compositeNode));

  ApplyFilterGraphToPixmap(pixmap, graph, Transform2d(), Box2d::FromXYWH(0.0, 0.0, 8.0, 8.0));

  // (2,2) is outside the green region → should be transparent.
  EXPECT_THAT(GetPixel(pixmap, 2, 2), Rgba(0, 0, 0, 0));
  // (4,4) is inside the green region → should keep the red pixel.
  EXPECT_THAT(GetPixel(pixmap, 4, 4), Rgba(255, 0, 0, 255));
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

  ApplyFilterGraphToPixmap(pixmap, graph, Transform2d(), std::nullopt);

  const Pixel result = GetPixel(pixmap, 4, 4);
  // Multiply: white * 128/255 ≈ 128. Allow tolerance for rounding.
  EXPECT_THAT(result, Rgba(ChannelNear(128, 2), ChannelNear(128, 2), ChannelNear(128, 2), 255));
}

// Render a 2x2 feImage (left column red, right column blue) into an 8x4 pixmap. With no host
// transform the executor fits the 2x2 source into the pixmap with `preserveAspectRatio=xMidYMid
// meet` (scale 2, centered), placing it at device x in [2, 6), so device columns 2-3 map to the
// red source column and columns 4-5 map to the blue source column. `pixelated` selects the
// `image-rendering` sampling kernel.
tiny_skia::Pixmap RenderTwoColumnFeImage(bool pixelated, int width = 8, int height = 4) {
  auto maybePixmap = tiny_skia::Pixmap::fromSize(static_cast<std::uint32_t>(width),
                                                 static_cast<std::uint32_t>(height));
  EXPECT_TRUE(maybePixmap.has_value());
  tiny_skia::Pixmap pixmap = std::move(*maybePixmap);

  components::filter_primitive::Image image;
  // Straight-alpha RGBA, two columns: red then blue, duplicated across both rows.
  image.imageData = std::make_shared<const std::vector<uint8_t>>(std::initializer_list<uint8_t>{
      255, 0, 0, 255, 0, 0, 255, 255, 255, 0, 0, 255, 0, 0, 255, 255});
  image.imageWidth = 2;
  image.imageHeight = 2;
  image.imageRendering = pixelated ? ImageRendering::Pixelated : ImageRendering::Auto;

  components::FilterGraph graph;
  components::FilterNode node;
  node.primitive = std::move(image);
  graph.nodes.push_back(std::move(node));

  ApplyFilterGraphToPixmap(pixmap, graph, Transform2d(), std::nullopt);
  return pixmap;
}

TEST(FilterGraphExecutorTest, FeImagePixelatedUsesNearestNeighborSharpEdge) {
  const tiny_skia::Pixmap pixmap = RenderTwoColumnFeImage(/*pixelated=*/true);

  // Nearest-neighbor: each device column resolves to exactly one source column, so the red/blue
  // boundary is sharp with no cross-color bleed.
  EXPECT_THAT(GetPixel(pixmap, 2, 1), Rgba(255, 0, 0, 255));
  EXPECT_THAT(GetPixel(pixmap, 3, 1), Rgba(255, 0, 0, 255));
  EXPECT_THAT(GetPixel(pixmap, 4, 1), Rgba(0, 0, 255, 255));
  EXPECT_THAT(GetPixel(pixmap, 5, 1), Rgba(0, 0, 255, 255));
}

TEST(FilterGraphExecutorTest, FeImageDefaultKernelBlendsAcrossEdge) {
  const tiny_skia::Pixmap pixmap = RenderTwoColumnFeImage(/*pixelated=*/false);

  // Default (Mitchell-bicubic) kernel: the columns straddling the red/blue boundary blend the two
  // source colors, so blue bleeds onto the red side and red onto the blue side. This is the
  // behavior nearest-neighbor replaces, proving the `image-rendering` flag selects the kernel.
  EXPECT_THAT(GetPixel(pixmap, 3, 1), Rgba(Gt(0), _, Gt(0), _));
  EXPECT_THAT(GetPixel(pixmap, 4, 1), Rgba(Gt(0), _, Gt(0), _));
}

TEST(FilterGraphExecutorTest, FeImagePixelatedSmoothsFromNearestIntegerScale) {
  const tiny_skia::Pixmap pixmap =
      RenderTwoColumnFeImage(/*pixelated=*/true, /*width=*/5, /*height=*/5);

  EXPECT_THAT(GetPixel(pixmap, 2, 2), Rgba(Gt(0), 0, Gt(0), 255));
}

TEST(FilterGraphExecutorTest, FeImageRejectsTrailingPayloadBytes) {
  auto maybePixmap = tiny_skia::Pixmap::fromSize(4, 4);
  ASSERT_TRUE(maybePixmap.has_value());
  tiny_skia::Pixmap pixmap = std::move(*maybePixmap);

  components::filter_primitive::Image image;
  image.imageData = std::make_shared<const std::vector<uint8_t>>(
      std::initializer_list<uint8_t>{255, 0, 0, 255, 17});
  image.imageWidth = 1;
  image.imageHeight = 1;

  components::FilterGraph graph;
  components::FilterNode node;
  node.primitive = std::move(image);
  graph.nodes.push_back(std::move(node));

  ApplyFilterGraphToPixmap(pixmap, graph, Transform2d(), std::nullopt);

  EXPECT_THAT(GetPixel(pixmap, 2, 2), Rgba(0, 0, 0, 0));
}

// ---------------------------------------------------------------------------
// Per-primitive color-interpolation-filters
// ---------------------------------------------------------------------------

/// Builds a source graphic with saturated colors, mid-tones, and partial alpha, so the results
/// below depend on both segments of the sRGB transfer function.
tiny_skia::Pixmap MakeColorSourceGraphic() {
  auto maybePixmap = tiny_skia::Pixmap::fromSize(24, 24);
  EXPECT_TRUE(maybePixmap.has_value());
  tiny_skia::Pixmap pixmap = std::move(*maybePixmap);

  for (int y = 0; y < 24; ++y) {
    for (int x = 0; x < 24; ++x) {
      const int alpha = 40 + (x * 9 + y * 3) % 216;
      const auto scale = [&](int value) { return static_cast<std::uint8_t>(value * alpha / 255); };
      SetPixel(pixmap, x, y,
               Pixel{scale((x * 11) % 256), scale((y * 7 + 3) % 256), scale((x * 5 + y * 13) % 256),
                     static_cast<std::uint8_t>(alpha)});
    }
  }

  return pixmap;
}

components::FilterNode BlurNodeWithSpace(std::optional<ColorInterpolationFilters> space) {
  components::FilterNode node;
  node.inputs.push_back(components::FilterInput{});  // previous
  node.primitive = components::filter_primitive::GaussianBlur{
      .stdDeviationX = 1.5,
      .stdDeviationY = 1.5,
  };
  node.colorInterpolationFilters = space;
  return node;
}

components::FilterNode SaturateNodeWithSpace(std::optional<ColorInterpolationFilters> space) {
  components::FilterNode node;
  node.inputs.push_back(components::FilterInput{});  // previous
  node.primitive = components::filter_primitive::ColorMatrix{
      .type = components::filter_primitive::ColorMatrix::Type::Saturate,
      .values = {0.4},
  };
  node.colorInterpolationFilters = space;
  return node;
}

/// Runs each node as its own single-primitive graph, feeding one result into the next. Because a
/// graph converts into its interpolation space on entry and back out on exit, this reproduces the
/// per-primitive conversion schedule exactly, whatever the executor does inside a longer graph.
tiny_skia::Pixmap RunNodesOneGraphAtATime(const tiny_skia::Pixmap& source,
                                          std::vector<components::FilterNode> nodes) {
  tiny_skia::Pixmap pixmap = source;
  for (components::FilterNode& node : nodes) {
    components::FilterGraph graph;
    graph.nodes.push_back(std::move(node));
    ApplyFilterGraphToPixmap(pixmap, graph, Transform2d(), std::nullopt);
  }
  return pixmap;
}

tiny_skia::Pixmap RunNodesAsOneGraph(const tiny_skia::Pixmap& source,
                                     std::vector<components::FilterNode> nodes) {
  tiny_skia::Pixmap pixmap = source;
  components::FilterGraph graph;
  graph.nodes = std::move(nodes);
  ApplyFilterGraphToPixmap(pixmap, graph, Transform2d(), std::nullopt);
  return pixmap;
}

/// Largest absolute per-channel difference between two same-sized pixmaps.
int MaxChannelDelta(const tiny_skia::Pixmap& lhs, const tiny_skia::Pixmap& rhs) {
  const auto a = lhs.data();
  const auto b = rhs.data();
  EXPECT_EQ(a.size(), b.size());

  int worst = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    worst = std::max(worst, std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])));
  }
  return worst;
}

/// Asserts that a graph agrees with the same primitives run one filter at a time.
///
/// The two are the same computation up to one source of difference: the per-primitive reference
/// hands each result back as an 8-bit pixmap, so it quantizes between primitives where a single
/// graph keeps float intermediates. That is worth one unit per channel. A larger difference means
/// the graph converted color spaces somewhere the per-primitive schedule did not, which is the
/// property these tests are protecting.
void ExpectMatchesPerPrimitiveRun(const tiny_skia::Pixmap& combined,
                                  const tiny_skia::Pixmap& perPrimitive) {
  EXPECT_LE(MaxChannelDelta(combined, perPrimitive), 1);
}

// A primitive that interpolates in sRGB between two that interpolate in linearRGB has to convert
// on both crossings, which is the schedule running each primitive as its own filter produces.
TEST(FilterGraphExecutorTest, SrgbPrimitiveBetweenLinearPrimitivesMatchesOneGraphPerPrimitive) {
  const tiny_skia::Pixmap source = MakeColorSourceGraphic();

  const auto nodes = [] {
    std::vector<components::FilterNode> nodes;
    nodes.push_back(BlurNodeWithSpace(ColorInterpolationFilters::LinearRGB));
    nodes.push_back(SaturateNodeWithSpace(ColorInterpolationFilters::SRGB));
    nodes.push_back(BlurNodeWithSpace(ColorInterpolationFilters::LinearRGB));
    return nodes;
  };

  const tiny_skia::Pixmap combined = RunNodesAsOneGraph(source, nodes());
  const tiny_skia::Pixmap perPrimitive = RunNodesOneGraphAtATime(source, nodes());

  ExpectMatchesPerPrimitiveRun(combined, perPrimitive);
}

// The mirror image: a linearRGB primitive between two sRGB ones.
TEST(FilterGraphExecutorTest, LinearPrimitiveBetweenSrgbPrimitivesMatchesOneGraphPerPrimitive) {
  const tiny_skia::Pixmap source = MakeColorSourceGraphic();

  const auto nodes = [] {
    std::vector<components::FilterNode> nodes;
    nodes.push_back(BlurNodeWithSpace(ColorInterpolationFilters::SRGB));
    nodes.push_back(SaturateNodeWithSpace(ColorInterpolationFilters::LinearRGB));
    nodes.push_back(BlurNodeWithSpace(ColorInterpolationFilters::SRGB));
    return nodes;
  };

  const tiny_skia::Pixmap combined = RunNodesAsOneGraph(source, nodes());
  const tiny_skia::Pixmap perPrimitive = RunNodesOneGraphAtATime(source, nodes());

  ExpectMatchesPerPrimitiveRun(combined, perPrimitive);
}

// When every primitive agrees on the interpolation space, the intermediate results stay in that
// space instead of being converted back out and in around every primitive. That removes rounding
// rather than adding it, so the result still tracks the per-primitive schedule.
TEST(FilterGraphExecutorTest, UniformLinearGraphStaysWithinRoundingOfOneGraphPerPrimitive) {
  const tiny_skia::Pixmap source = MakeColorSourceGraphic();

  const auto nodes = [] {
    std::vector<components::FilterNode> nodes;
    nodes.push_back(BlurNodeWithSpace(ColorInterpolationFilters::LinearRGB));
    nodes.push_back(SaturateNodeWithSpace(ColorInterpolationFilters::LinearRGB));
    nodes.push_back(BlurNodeWithSpace(ColorInterpolationFilters::LinearRGB));
    return nodes;
  };

  const tiny_skia::Pixmap combined = RunNodesAsOneGraph(source, nodes());
  const tiny_skia::Pixmap perPrimitive = RunNodesOneGraphAtATime(source, nodes());

  ExpectMatchesPerPrimitiveRun(combined, perPrimitive);
}

// Interpolating in sRGB throughout converts nowhere at all, in either arrangement.
TEST(FilterGraphExecutorTest, UniformSrgbGraphMatchesOneGraphPerPrimitive) {
  const tiny_skia::Pixmap source = MakeColorSourceGraphic();

  const auto nodes = [] {
    std::vector<components::FilterNode> nodes;
    nodes.push_back(BlurNodeWithSpace(ColorInterpolationFilters::SRGB));
    nodes.push_back(SaturateNodeWithSpace(ColorInterpolationFilters::SRGB));
    nodes.push_back(BlurNodeWithSpace(ColorInterpolationFilters::SRGB));
    return nodes;
  };

  const tiny_skia::Pixmap combined = RunNodesAsOneGraph(source, nodes());
  const tiny_skia::Pixmap perPrimitive = RunNodesOneGraphAtATime(source, nodes());

  ExpectMatchesPerPrimitiveRun(combined, perPrimitive);
}

// The per-primitive space has to actually take effect: saturating in sRGB and saturating in
// linearRGB are different computations, so the two graphs must disagree, and by much more than
// the rounding the tests above tolerate. The measured separation is 36/255; the bound sits well
// clear of both that and the single unit that a same-schedule comparison produces, so this fails
// loudly if the per-primitive space is ever ignored.
TEST(FilterGraphExecutorTest, PerPrimitiveColorSpaceChangesTheResult) {
  const tiny_skia::Pixmap source = MakeColorSourceGraphic();

  std::vector<components::FilterNode> srgbNodes;
  srgbNodes.push_back(BlurNodeWithSpace(ColorInterpolationFilters::LinearRGB));
  srgbNodes.push_back(SaturateNodeWithSpace(ColorInterpolationFilters::SRGB));

  std::vector<components::FilterNode> linearNodes;
  linearNodes.push_back(BlurNodeWithSpace(ColorInterpolationFilters::LinearRGB));
  linearNodes.push_back(SaturateNodeWithSpace(ColorInterpolationFilters::LinearRGB));

  const tiny_skia::Pixmap srgbInterior = RunNodesAsOneGraph(source, std::move(srgbNodes));
  const tiny_skia::Pixmap linearInterior = RunNodesAsOneGraph(source, std::move(linearNodes));

  EXPECT_GT(MaxChannelDelta(srgbInterior, linearInterior), 16);
}

/// A one-node graph built from \p primitive with the filter's default (previous) input.
components::FilterGraph SinglePrimitiveGraph(components::FilterPrimitive primitive) {
  components::FilterGraph graph;
  components::FilterNode node;
  node.primitive = std::move(primitive);
  graph.nodes.push_back(std::move(node));
  return graph;
}

// tiny-skia's float morphology writes nothing when its three float working buffers would exceed
// 64 MiB. The work budget refuses every morphology raster that large, so an input margin can grow a
// morphology raster only to a size the budget already bounds at under a quarter of that cap.
TEST(FilterInputMarginTest, MorphologyWorkBudgetStaysBelowTheMorphologyBufferCap) {
  using namespace components;
  constexpr std::uint64_t kMorphologyCapPixels = 64u * 1024u * 1024u / (3u * 4u * sizeof(float));
  const FilterGraph graph = SinglePrimitiveGraph(
      filter_primitive::Morphology{filter_primitive::Morphology::Operator::Dilate, 1.0, 1.0});
  EXPECT_FALSE(FilterGraphFitsExecutionBudget(graph, kMorphologyCapPixels / 4,
                                              FilterMemoryModel::CpuFloatNamedResults));
}

TEST(FilterInputMarginTest, CanReserveAgreesWithReserveWithoutLatching) {
  using namespace components;
  const FilterGraph graph = SinglePrimitiveGraph(filter_primitive::GaussianBlur{2.0, 2.0});
  FilterExecutionBudget budget;
  EXPECT_FALSE(budget.canReserve(graph, 16, FilterMemoryModel::CpuFloatNamedResults,
                                 kMaximumFilterFrameBytes + 1));
  EXPECT_THAT(budget.rejectionReason(), testing::Eq(FilterExecutionBudget::RejectionReason::None));
  EXPECT_EQ(budget.executions(), 0u);

  ASSERT_TRUE(budget.canReserve(graph, 16, FilterMemoryModel::CpuFloatNamedResults, 64));
  auto reservation = budget.reserve(graph, 16, FilterMemoryModel::CpuFloatNamedResults, 64);
  ASSERT_TRUE(reservation.has_value());
  EXPECT_EQ(budget.executions(), 1u);

  EXPECT_FALSE(budget.reserve(graph, 16, FilterMemoryModel::CpuFloatNamedResults,
                              kMaximumFilterFrameBytes + 1));
  EXPECT_FALSE(budget.canReserve(graph, 16, FilterMemoryModel::CpuFloatNamedResults, 64))
      << "a latched rejection closes the frame for both";
}

TEST(FilterInputMarginTest, LayoutAxisExtendsByReachAndCaptureWithinTheRegion) {
  using testing::FieldsAre;
  // Region reaches 10 pixels before and 36 after a 64-pixel viewport.
  EXPECT_THAT(LayoutFilterInputAxis({-10.0, 100.0}, 3.0, std::nullopt, 64), FieldsAre(3, 70));
  // A capture rectangle beyond the reach widens only its own side, clamped to the region.
  EXPECT_THAT(LayoutFilterInputAxis({-10.0, 100.0}, 3.0, std::pair(-40.0, -20.0), 64),
              FieldsAre(10, 77));
  // Nothing extends past an edge the region does not cross.
  EXPECT_THAT(LayoutFilterInputAxis({0.0, 64.0}, 3.0, std::nullopt, 64), FieldsAre(0, 64));
}

TEST(FilterInputMarginTest, InputBufferFollowsGraphReach) {
  using namespace components;
  const std::optional<Box2d> region = Box2d::FromXYWH(-50, -50, 200, 200);
  using testing::FieldsAre;
  const auto layout = [&](const FilterGraph& graph) {
    return ComputeFilterInputBuffer(graph, region, Transform2d(), 64, 64);
  };
  // Three standard deviations plus a rounding pixel.
  EXPECT_THAT(layout(SinglePrimitiveGraph(filter_primitive::GaussianBlur{2.0, 2.0})),
              FieldsAre(78, 78, 7, 7));
  // Point-wise primitives keep the viewport.
  EXPECT_THAT(layout(SinglePrimitiveGraph(filter_primitive::ColorMatrix{})),
              FieldsAre(64, 64, 0, 0));
  // Turbulence keeps a viewport-aligned capture.
  EXPECT_THAT(layout(SinglePrimitiveGraph(filter_primitive::Turbulence{})),
              FieldsAre(64, 64, 0, 0));
  // Wrapping edges read the opposite side of the input, so the capture covers the whole region.
  filter_primitive::GaussianBlur wrap{2.0, 2.0};
  wrap.edgeMode = filter_primitive::GaussianBlur::EdgeMode::Wrap;
  EXPECT_THAT(layout(SinglePrimitiveGraph(wrap)), FieldsAre(200, 200, 50, 50));
}

TEST(FilterInputMarginTest, TileCaptureIncludesItsInputsReach) {
  using namespace components;
  FilterGraph graph;
  FilterNode offset;
  offset.primitive = filter_primitive::Offset{30.0, 30.0};
  offset.x = Lengthd(-60, Lengthd::Unit::None);
  offset.y = Lengthd(-60, Lengthd::Unit::None);
  offset.width = Lengthd(20, Lengthd::Unit::None);
  offset.height = Lengthd(20, Lengthd::Unit::None);
  offset.result = RcString("t");
  FilterNode tile;
  tile.primitive = filter_primitive::Tile{};
  tile.inputs.push_back(FilterInput(FilterInput::Named{RcString("t")}));
  graph.nodes = {offset, tile};
  // The tile repeats the offset's subregion at -60..-40, which reads source pixels 30 farther
  // out, plus a rounding pixel: the capture reaches 91 pixels before the viewport.
  const FilterInputBuffer buffer =
      ComputeFilterInputBuffer(graph, Box2d::FromXYWH(-200, -200, 464, 464), Transform2d(), 64, 64);
  // After the viewport the capture extends by the graph's own 31-pixel reach.
  EXPECT_THAT(buffer, testing::FieldsAre(186, 186, 91, 91));
}

}  // namespace
}  // namespace donner::svg
