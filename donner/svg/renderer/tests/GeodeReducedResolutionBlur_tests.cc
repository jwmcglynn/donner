/// @file
/// Bounded-error and counter tests for Geode's reduced-resolution Gaussian blur.
///
/// A blur with a large device-space deviation runs on an image averaged down by a power of two,
/// then is bilinearly enlarged back. These tests render the same filtered content with the
/// reduction enabled and disabled on one device and bound the difference, and check the frame
/// counters that report the reduction.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/css/Color.h"
#include "donner/svg/components/filter/FilterGraph.h"
#include "donner/svg/properties/PaintServer.h"
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/StrokeParams.h"
#include "donner/svg/renderer/geode/GeodeCounters.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeFilterEngine.h"

namespace donner::svg {
namespace {

namespace fp = components::filter_primitive;
using components::FilterGraph;
using components::FilterInput;
using components::FilterNode;
using components::FilterStandardInput;

/// Largest premultiplied channel difference, in 8-bit levels, a reduced blur may differ from the
/// full-resolution blur at any pixel.
constexpr int kMaximumChannelError = 3;

/// Largest mean premultiplied channel difference, in 8-bit levels, over the pixels either result
/// covers.
constexpr double kMaximumMeanChannelError = 0.3;

/// Mean tolerance for a blur along one axis only. Averaging a block keeps where its content's mass
/// is only to within the block, and with the other axis unblurred nothing averages that away
/// between rows: a row of the checker whose first cell is opaque and the next row, whose first
/// cell is translucent, both reduce to the same blocks, so each row's blurred edge sits up to a
/// texel from where the full-resolution blur puts it. A blur along both axes averages the rows
/// and keeps to \ref kMaximumMeanChannelError.
constexpr double kMaximumOneAxisMeanChannelError = 1.0;

constexpr int kViewport = 320;

/// Content drawn inside the filter layer.
using Scene = std::function<void(RendererGeode&)>;

PaintParams SolidFill(const css::RGBA& rgba) {
  PaintParams paint;
  paint.fill = PaintServer::Solid{css::Color(rgba)};
  paint.fillOpacity = 1.0;
  paint.opacity = 1.0;
  return paint;
}

/// Difference between two straight-alpha snapshots, measured on premultiplied channels so that
/// nearly transparent pixels do not magnify it.
struct ImageError {
  int maximum = 0;                    //!< Largest channel difference, in 8-bit levels.
  double mean = 0.0;                  //!< Mean channel difference over covered pixels.
  size_t coveredPixels = 0;           //!< Pixels with nonzero alpha in either image.
  Vector2i worst = Vector2i::Zero();  //!< Pixel holding the largest difference.
};

ImageError Compare(const RendererBitmap& actual, const RendererBitmap& expected) {
  ImageError error;
  if (actual.dimensions != expected.dimensions) {
    error.maximum = 255;
    return error;
  }
  uint64_t sum = 0;
  for (int y = 0; y < actual.dimensions.y; ++y) {
    const uint8_t* a = actual.pixels.data() + y * actual.rowBytes;
    const uint8_t* e = expected.pixels.data() + y * expected.rowBytes;
    for (int x = 0; x < actual.dimensions.x; ++x, a += 4, e += 4) {
      if (a[3] == 0 && e[3] == 0) {
        continue;
      }
      ++error.coveredPixels;
      for (int channel = 0; channel < 4; ++channel) {
        const int actualValue = channel == 3 ? a[3] : (a[channel] * a[3] + 127) / 255;
        const int expectedValue = channel == 3 ? e[3] : (e[channel] * e[3] + 127) / 255;
        const int difference = std::abs(actualValue - expectedValue);
        if (difference > error.maximum) {
          error.maximum = difference;
          error.worst = Vector2i(x, y);
        }
        sum += difference;
      }
    }
  }
  if (error.coveredPixels != 0) {
    error.mean = static_cast<double>(sum) / static_cast<double>(error.coveredPixels * 4);
  }
  return error;
}

/// One blur primitive in a single-node graph.
FilterGraph BlurGraph(fp::GaussianBlur blur, bool linearRgb = false) {
  FilterGraph graph;
  graph.colorInterpolationFilters =
      linearRgb ? ColorInterpolationFilters::LinearRGB : ColorInterpolationFilters::SRGB;
  FilterNode node;
  node.primitive = blur;
  graph.nodes.push_back(node);
  return graph;
}

/// A glow: the source's alpha blurred, tinted with a flood, and the unblurred source merged over
/// it.
FilterGraph GlowGraph(double deviation) {
  FilterGraph graph;
  graph.colorInterpolationFilters = ColorInterpolationFilters::SRGB;
  FilterNode blur;
  blur.primitive = fp::GaussianBlur{.stdDeviationX = deviation, .stdDeviationY = deviation};
  blur.inputs = {FilterStandardInput::SourceAlpha};
  blur.result = RcString("blurred");
  graph.nodes.push_back(blur);
  FilterNode flood;
  flood.primitive =
      fp::Flood{.floodColor = css::Color(css::RGBA(255, 220, 64, 255)), .floodOpacity = 0.8};
  graph.nodes.push_back(flood);
  FilterNode tint;
  tint.primitive = fp::Composite{.op = fp::Composite::Operator::In};
  tint.inputs = {FilterInput(), FilterInput::Named{RcString("blurred")}};
  tint.result = RcString("glow");
  graph.nodes.push_back(tint);
  FilterNode merge;
  merge.primitive = fp::Merge{};
  merge.inputs = {FilterInput::Named{RcString("glow")}, FilterStandardInput::SourceGraphic};
  graph.nodes.push_back(merge);
  return graph;
}

void DrawSquare(RendererGeode& renderer) {
  renderer.setPaint(SolidFill(css::RGBA(46, 139, 87, 255)));
  renderer.drawRect(Box2d({100, 100}, {220, 220}), StrokeParams{});
}

void DrawDisc(RendererGeode& renderer) {
  renderer.setPaint(SolidFill(css::RGBA(220, 40, 160, 255)));
  renderer.drawEllipse(Box2d({90, 110}, {230, 210}), StrokeParams{});
}

/// One-pixel lines: the highest-frequency content, where averaging down aliases the most.
void DrawThinLines(RendererGeode& renderer) {
  renderer.setPaint(SolidFill(css::RGBA(255, 255, 255, 255)));
  for (int x = 100; x < 220; x += 7) {
    renderer.drawRect(Box2d({static_cast<double>(x), 100}, {x + 1.0, 220}), StrokeParams{});
  }
}

/// Content touching three edges of the filter region, so the blur spills past the raster edge.
void DrawEdgeTouching(RendererGeode& renderer) {
  renderer.setPaint(SolidFill(css::RGBA(30, 90, 200, 255)));
  renderer.drawRect(Box2d({-4, -4}, {kViewport + 4.0, 150}), StrokeParams{});
  renderer.setPaint(SolidFill(css::RGBA(240, 200, 40, 200)));
  renderer.drawEllipse(Box2d({250, 200}, {kViewport + 40.0, 340}), StrokeParams{});
}

/// A two-pixel checkerboard in two colors. Its average is what a wide blur converges to, so the
/// colors keep that average off a half-level rounding boundary, where the two results would round
/// apart by a level over the whole blurred interior while agreeing to within float precision.
void DrawChecker(RendererGeode& renderer) {
  for (int y = 120; y < 200; y += 2) {
    for (int x = 120; x < 200; x += 2) {
      const bool odd = ((x + y) / 2) % 2 != 0;
      renderer.setPaint(odd ? SolidFill(css::RGBA(250, 130, 10, 255))
                            : SolidFill(css::RGBA(0, 60, 240, 151)));
      renderer.drawRect(Box2d({static_cast<double>(x), static_cast<double>(y)}, {x + 2.0, y + 2.0}),
                        StrokeParams{});
    }
  }
}

class GeodeReducedResolutionBlurTest : public testing::Test {
protected:
  static std::shared_ptr<geode::GeodeDevice> SharedDevice() {
    static auto device = std::shared_ptr<geode::GeodeDevice>(geode::GeodeDevice::CreateHeadless());
    return device;
  }

  void SetUp() override { ASSERT_THAT(SharedDevice(), testing::NotNull()); }

  void TearDown() override {
    SharedDevice()->filterEngine().setReducedResolutionBlurForTesting(true);
  }

  /// Renders \p scene through \p graph over the whole viewport scaled by \p scale.
  /// @param reduced Whether reduced-resolution blur is allowed.
  /// @param counters Receives the frame's counters when non-null.
  /// @param size Square viewport size in pixels.
  RendererBitmap render(const FilterGraph& graph, const Scene& scene, bool reduced,
                        geode::GeodeCounters* counters = nullptr, double scale = 1.0,
                        const std::shared_ptr<geode::GeodeDevice>& device = SharedDevice(),
                        int size = kViewport) {
    device->filterEngine().setReducedResolutionBlurForTesting(reduced);
    RendererGeode renderer(device);
    RenderViewport viewport;
    viewport.size = Vector2d(size, size);
    viewport.devicePixelRatio = 1.0;
    renderer.beginFrame(viewport);
    renderer.setTransform(Transform2d::Scale(scale));
    renderer.pushFilterLayer(graph, Box2d({0, 0}, {size / scale, size / scale}));
    renderer.setTransform(Transform2d());
    scene(renderer);
    renderer.popFilterLayer();
    renderer.endFrame();
    if (counters != nullptr) {
      *counters = renderer.lastFrameTimings().counters;
    }
    return renderer.takeSnapshot();
  }

  /// Renders with and without the reduction, checks the counters say which ran, and returns the
  /// difference.
  ImageError measure(const FilterGraph& graph, const Scene& scene, const std::string& label,
                     double scale = 1.0) {
    geode::GeodeCounters reducedCounters;
    geode::GeodeCounters fullCounters;
    const RendererBitmap reduced = render(graph, scene, true, &reducedCounters, scale);
    const RendererBitmap full = render(graph, scene, false, &fullCounters, scale);
    EXPECT_FALSE(reduced.empty());
    EXPECT_FALSE(full.empty());
    EXPECT_THAT(reducedCounters.reducedResolutionBlurs, testing::Ge(1u)) << label;
    EXPECT_THAT(fullCounters.reducedResolutionBlurs, testing::Eq(0u)) << label;
    EXPECT_THAT(reducedCounters.filterBlurTaps * 2, testing::Lt(fullCounters.filterBlurTaps))
        << label;
    const ImageError error = Compare(reduced, full);
    std::cout << "reduced blur error " << label << ": max " << error.maximum << " at "
              << error.worst << " mean " << error.mean << " over " << error.coveredPixels
              << " px, filter pixels " << reducedCounters.filterPixels << " vs "
              << fullCounters.filterPixels << ", blur taps " << reducedCounters.filterBlurTaps
              << " vs " << fullCounters.filterBlurTaps << "\n";
    return error;
  }
};

struct BlurCase {
  const char* name;
  double deviationX;
  double deviationY;
  fp::GaussianBlur::EdgeMode edgeMode = fp::GaussianBlur::EdgeMode::None;
  bool linearRgb = false;
  double meanTolerance = kMaximumMeanChannelError;
};

void PrintTo(const BlurCase& value, std::ostream* output) {
  *output << value.name;
}

class ReducedBlurErrorTest : public GeodeReducedResolutionBlurTest,
                             public testing::WithParamInterface<BlurCase> {};

TEST_P(ReducedBlurErrorTest, StaysWithinToleranceOfFullResolution) {
  const BlurCase& test = GetParam();
  const FilterGraph graph = BlurGraph(fp::GaussianBlur{.stdDeviationX = test.deviationX,
                                                       .stdDeviationY = test.deviationY,
                                                       .edgeMode = test.edgeMode},
                                      test.linearRgb);
  for (const auto& [sceneName, scene] :
       std::vector<std::pair<std::string, Scene>>{{"square", DrawSquare},
                                                  {"disc", DrawDisc},
                                                  {"thin_lines", DrawThinLines},
                                                  {"checker", DrawChecker},
                                                  {"edge_touching", DrawEdgeTouching}}) {
    SCOPED_TRACE(sceneName);
    const ImageError error = measure(graph, scene, std::string(test.name) + "/" + sceneName);
    EXPECT_THAT(error.maximum, testing::Le(kMaximumChannelError));
    EXPECT_THAT(error.mean, testing::Le(test.meanTolerance));
  }
}

INSTANTIATE_TEST_SUITE_P(
    Deviations, ReducedBlurErrorTest,
    testing::Values(BlurCase{"sigma6", 6, 6}, BlurCase{"sigma8", 8, 8}, BlurCase{"sigma12", 12, 12},
                    BlurCase{"sigma16", 16, 16}, BlurCase{"sigma24", 24, 24},
                    BlurCase{"sigma40", 40, 40}, BlurCase{"sigma64", 64, 64},
                    BlurCase{"anisotropic_x", 20, 3}, BlurCase{"anisotropic_y", 1.5, 30},
                    BlurCase{"one_axis", 24, 0, fp::GaussianBlur::EdgeMode::None, false,
                             kMaximumOneAxisMeanChannelError},
                    BlurCase{"linear_rgb", 16, 16, fp::GaussianBlur::EdgeMode::None, true}),
    [](const testing::TestParamInfo<BlurCase>& info) { return std::string(info.param.name); });

// Device-space deviation decides the reduction, so a small user-space blur under a large
// transform is reduced too.
TEST_F(GeodeReducedResolutionBlurTest, ScaledUserSpaceBlurIsReducedWithinTolerance) {
  const FilterGraph graph = BlurGraph(fp::GaussianBlur{.stdDeviationX = 4, .stdDeviationY = 4});
  const ImageError error = measure(graph, DrawDisc, "user_sigma4_scale3", 3.0);
  EXPECT_THAT(error.maximum, testing::Le(kMaximumChannelError));
  EXPECT_THAT(error.mean, testing::Le(kMaximumMeanChannelError));
}

TEST_F(GeodeReducedResolutionBlurTest, DropShadowBlurIsReducedWithinTolerance) {
  FilterGraph graph;
  graph.colorInterpolationFilters = ColorInterpolationFilters::SRGB;
  FilterNode node;
  node.primitive = fp::DropShadow{.dx = 6,
                                  .dy = 8,
                                  .stdDeviationX = 14,
                                  .stdDeviationY = 14,
                                  .floodColor = css::Color(css::RGBA(0, 0, 0, 255)),
                                  .floodOpacity = 0.6};
  graph.nodes.push_back(node);
  const ImageError error = measure(graph, DrawSquare, "drop_shadow_sigma14");
  EXPECT_THAT(error.maximum, testing::Le(kMaximumChannelError));
  EXPECT_THAT(error.mean, testing::Le(kMaximumMeanChannelError));
}

// The glow is reduced but the source merged over it is not: wherever the opaque source covers the
// glow, the two results are identical.
TEST_F(GeodeReducedResolutionBlurTest, UnblurredSourceInAGlowStaysFullResolution) {
  const FilterGraph graph = GlowGraph(16);
  const ImageError error = measure(graph, DrawChecker, "glow_sigma16_checker");
  EXPECT_THAT(error.maximum, testing::Le(kMaximumChannelError));
  EXPECT_THAT(error.mean, testing::Le(kMaximumMeanChannelError));

  const RendererBitmap reduced = render(graph, DrawChecker, true);
  const RendererBitmap full = render(graph, DrawChecker, false);
  ASSERT_EQ(reduced.dimensions, full.dimensions);
  size_t opaqueSourcePixels = 0;
  for (int y = 120; y < 200; ++y) {
    for (int x = 120; x < 200; ++x) {
      if (((x / 2 * 2 + y / 2 * 2) / 2) % 2 == 0) {
        continue;  // The translucent checker color lets the glow through.
      }
      ++opaqueSourcePixels;
      const size_t offset = y * full.rowBytes + x * 4;
      for (int channel = 0; channel < 4; ++channel) {
        ASSERT_EQ(reduced.pixels[offset + channel], full.pixels[offset + channel])
            << "pixel " << x << "," << y << " channel " << channel;
      }
    }
  }
  EXPECT_THAT(opaqueSourcePixels, testing::Gt(1000u));
}

// Below the threshold nothing changes: the reduced path is not taken and the output is bitwise
// the full-resolution one.
TEST_F(GeodeReducedResolutionBlurTest, SmallBlurIsNotReduced) {
  // Below six on both axes nothing reduces, and a reduction by two along one axis only would save
  // too little to pay for its two extra passes.
  for (const auto& [deviationX, deviationY] : std::vector<std::pair<double, double>>{
           {2.0, 2.0}, {4.0, 4.0}, {5.9, 5.9}, {8.0, 0.0}, {3.0, 10.0}}) {
    SCOPED_TRACE(testing::Message() << deviationX << "," << deviationY);
    const FilterGraph graph =
        BlurGraph(fp::GaussianBlur{.stdDeviationX = deviationX, .stdDeviationY = deviationY});
    geode::GeodeCounters reducedCounters;
    geode::GeodeCounters fullCounters;
    const RendererBitmap reduced = render(graph, DrawDisc, true, &reducedCounters);
    const RendererBitmap full = render(graph, DrawDisc, false, &fullCounters);
    EXPECT_THAT(reducedCounters.reducedResolutionBlurs, testing::Eq(0u));
    EXPECT_THAT(reducedCounters.filterPixels, testing::Eq(fullCounters.filterPixels));
    EXPECT_THAT(fullCounters.filterPixels, testing::Gt(0u));
    EXPECT_THAT(Compare(reduced, full).maximum, testing::Eq(0));
  }
}

// Only transparent edges reduce: a wrapped reduced texel at the seam would average both edges, and
// duplicated edges at full resolution clamp each pass's output, which a reduced blur would not
// reproduce where content touches the edge.
TEST_F(GeodeReducedResolutionBlurTest, WrappedAndDuplicatedEdgeBlursAreNotReduced) {
  for (const fp::GaussianBlur::EdgeMode edgeMode :
       {fp::GaussianBlur::EdgeMode::Wrap, fp::GaussianBlur::EdgeMode::Duplicate}) {
    SCOPED_TRACE(static_cast<int>(edgeMode));
    const FilterGraph graph =
        BlurGraph(fp::GaussianBlur{.stdDeviationX = 20, .stdDeviationY = 20, .edgeMode = edgeMode});
    geode::GeodeCounters counters;
    const RendererBitmap reduced = render(graph, DrawEdgeTouching, true, &counters);
    EXPECT_FALSE(reduced.empty());
    EXPECT_THAT(counters.reducedResolutionBlurs, testing::Eq(0u));
    EXPECT_THAT(Compare(reduced, render(graph, DrawEdgeTouching, false)).maximum, testing::Eq(0));
  }
}

// The counters report the filter's work. A 320x320 layer blurred at deviation 16 is reduced by four
// on both axes. At full resolution each axis is one pass of the three-box cascade over the layer,
// whose 30-texel window reads 89 texels per output texel. Reduced, the layer is averaged down to
// 82x82 (80 blocks plus an apron block each side, 16 reads each), blurred there with a
// 7.52-texel spline per axis (25 reads each), and enlarged back to 320x320 (4 reads each).
TEST_F(GeodeReducedResolutionBlurTest, CountersReportReducedFilterWork) {
  const FilterGraph graph = BlurGraph(fp::GaussianBlur{.stdDeviationX = 16, .stdDeviationY = 16});
  geode::GeodeCounters reduced;
  geode::GeodeCounters full;
  render(graph, DrawSquare, true, &reduced);
  render(graph, DrawSquare, false, &full);
  constexpr uint64_t kLayer = uint64_t{kViewport} * kViewport;
  // 320 / 4 blocks plus one block of apron on either side.
  constexpr uint64_t kReducedLayer = uint64_t{82} * 82;
  EXPECT_THAT(reduced.reducedResolutionBlurs, testing::Eq(1u));
  EXPECT_THAT(full.reducedResolutionBlurs, testing::Eq(0u));
  EXPECT_THAT(full.filterBlurTaps, testing::Eq(2 * kLayer * 89));
  EXPECT_THAT(reduced.filterBlurTaps,
              testing::Eq(kReducedLayer * 16 + 2 * kReducedLayer * 25 + kLayer * 4));
  // Texels written: the clip and resolve passes are shared; the two full-resolution blur passes
  // become one downsample and two blur passes on the reduced image plus one enlargement.
  const uint64_t shared = full.filterPixels - 2 * kLayer;
  EXPECT_THAT(reduced.filterPixels, testing::Eq(shared + 3 * kReducedLayer + kLayer));
}

// Tiled execution reduces each tile on the untiled execution's block grid, and the halo covers the
// reduced kernel, so the tiles reproduce the untiled reduced blur exactly. The odd 317-pixel layer
// leaves a partial block at its far edge, and the tile extents are chosen so tiles start at
// origins that are not multiples of the reduction factor, which exercises the blocks' phase.
TEST_F(GeodeReducedResolutionBlurTest, TiledReducedBlurMatchesUntiledReducedBlur) {
  constexpr int kSize = 317;
  for (const auto& [deviation, extent] :
       std::vector<std::pair<double, uint32_t>>{{10.0, 160}, {24.0, 240}}) {
    SCOPED_TRACE(deviation);
    const FilterGraph graph =
        BlurGraph(fp::GaussianBlur{.stdDeviationX = deviation, .stdDeviationY = deviation});
    std::shared_ptr<geode::GeodeDevice> tiledDevice = geode::GeodeDevice::CreateHeadless();
    ASSERT_THAT(tiledDevice, testing::NotNull());
    tiledDevice->filterEngine().setMaximumTileExtentForTesting(extent);
    for (const auto& [sceneName, scene] : std::vector<std::pair<std::string, Scene>>{
             {"disc", DrawDisc}, {"edge_touching", DrawEdgeTouching}}) {
      SCOPED_TRACE(sceneName);
      geode::GeodeCounters counters;
      const RendererBitmap tiled = render(graph, scene, true, &counters, 1.0, tiledDevice, kSize);
      const uint64_t tiles = tiledDevice->filterEngine().lastExecutionMemory().tileExecutions;
      ASSERT_THAT(tiles, testing::Gt(2u));
      EXPECT_THAT(counters.reducedResolutionBlurs, testing::Eq(tiles));
      const RendererBitmap untiled =
          render(graph, scene, true, nullptr, 1.0, SharedDevice(), kSize);
      EXPECT_THAT(Compare(tiled, untiled).maximum, testing::Eq(0));
      const ImageError error =
          Compare(tiled, render(graph, scene, false, nullptr, 1.0, SharedDevice(), kSize));
      std::cout << "reduced blur error tiled_sigma" << deviation << "/" << sceneName << ": max "
                << error.maximum << " at " << error.worst << " mean " << error.mean << "\n";
      EXPECT_THAT(error.maximum, testing::Le(kMaximumChannelError));
      EXPECT_THAT(error.mean, testing::Le(kMaximumMeanChannelError));
    }
  }
}

}  // namespace
}  // namespace donner::svg
