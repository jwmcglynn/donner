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
constexpr double kMaximumMeanChannelError = 0.5;

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
  int maximum = 0;          //!< Largest channel difference, in 8-bit levels.
  double mean = 0.0;        //!< Mean channel difference over covered pixels.
  size_t coveredPixels = 0;  //!< Pixels with nonzero alpha in either image.
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
        error.maximum = std::max(error.maximum, difference);
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
  flood.primitive = fp::Flood{.floodColor = css::Color(css::RGBA(255, 220, 64, 255)),
                              .floodOpacity = 0.8};
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

/// A two-pixel checkerboard in two colors.
void DrawChecker(RendererGeode& renderer) {
  for (int y = 120; y < 200; y += 2) {
    for (int x = 120; x < 200; x += 2) {
      const bool odd = ((x + y) / 2) % 2 != 0;
      renderer.setPaint(odd ? SolidFill(css::RGBA(255, 128, 0, 255))
                            : SolidFill(css::RGBA(0, 64, 255, 160)));
      renderer.drawRect(Box2d({static_cast<double>(x), static_cast<double>(y)},
                              {x + 2.0, y + 2.0}),
                        StrokeParams{});
    }
  }
}

class GeodeReducedResolutionBlurTest : public testing::Test {
protected:
  static std::shared_ptr<geode::GeodeDevice> SharedDevice() {
    static auto device =
        std::shared_ptr<geode::GeodeDevice>(geode::GeodeDevice::CreateHeadless());
    return device;
  }

  void SetUp() override { ASSERT_THAT(SharedDevice(), testing::NotNull()); }

  void TearDown() override { SharedDevice()->filterEngine().setReducedResolutionBlurForTesting(true); }

  /// Renders \p scene through \p graph over the whole viewport scaled by \p scale.
  /// @param reduced Whether reduced-resolution blur is allowed.
  /// @param counters Receives the frame's counters when non-null.
  RendererBitmap render(const FilterGraph& graph, const Scene& scene, bool reduced,
                        geode::GeodeCounters* counters = nullptr, double scale = 1.0,
                        const std::shared_ptr<geode::GeodeDevice>& device = SharedDevice()) {
    device->filterEngine().setReducedResolutionBlurForTesting(reduced);
    RendererGeode renderer(device);
    RenderViewport viewport;
    viewport.size = Vector2d(kViewport, kViewport);
    viewport.devicePixelRatio = 1.0;
    renderer.beginFrame(viewport);
    renderer.setTransform(Transform2d::Scale(scale));
    renderer.pushFilterLayer(graph, Box2d({0, 0}, {kViewport / scale, kViewport / scale}));
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
    EXPECT_THAT(reducedCounters.filterPixels, testing::Lt(fullCounters.filterPixels)) << label;
    const ImageError error = Compare(reduced, full);
    std::cout << "reduced blur error " << label << ": max " << error.maximum << " mean "
              << error.mean << " over " << error.coveredPixels << " px, filter pixels "
              << reducedCounters.filterPixels << " vs " << fullCounters.filterPixels << "\n";
    return error;
  }
};

struct BlurCase {
  const char* name;
  double deviationX;
  double deviationY;
  fp::GaussianBlur::EdgeMode edgeMode = fp::GaussianBlur::EdgeMode::None;
  bool linearRgb = false;
};

void PrintTo(const BlurCase& value, std::ostream* output) { *output << value.name; }

class ReducedBlurErrorTest : public GeodeReducedResolutionBlurTest,
                             public testing::WithParamInterface<BlurCase> {};

TEST_P(ReducedBlurErrorTest, StaysWithinToleranceOfFullResolution) {
  const BlurCase& test = GetParam();
  const FilterGraph graph = BlurGraph(fp::GaussianBlur{.stdDeviationX = test.deviationX,
                                                       .stdDeviationY = test.deviationY,
                                                       .edgeMode = test.edgeMode},
                                      test.linearRgb);
  for (const auto& [sceneName, scene] : std::vector<std::pair<std::string, Scene>>{
           {"square", DrawSquare},
           {"disc", DrawDisc},
           {"thin_lines", DrawThinLines},
           {"checker", DrawChecker}}) {
    SCOPED_TRACE(sceneName);
    const ImageError error = measure(graph, scene, std::string(test.name) + "/" + sceneName);
    EXPECT_THAT(error.maximum, testing::Le(kMaximumChannelError));
    EXPECT_THAT(error.mean, testing::Le(kMaximumMeanChannelError));
  }
}

INSTANTIATE_TEST_SUITE_P(
    Deviations, ReducedBlurErrorTest,
    testing::Values(BlurCase{"sigma8", 8, 8}, BlurCase{"sigma12", 12, 12},
                    BlurCase{"sigma16", 16, 16}, BlurCase{"sigma24", 24, 24},
                    BlurCase{"sigma40", 40, 40}, BlurCase{"sigma64", 64, 64},
                    BlurCase{"anisotropic_x", 20, 3}, BlurCase{"anisotropic_y", 1.5, 30},
                    BlurCase{"one_axis", 24, 0},
                    BlurCase{"duplicate_edges", 16, 16, fp::GaussianBlur::EdgeMode::Duplicate},
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
  for (double deviation : {2.0, 5.0, 7.9}) {
    SCOPED_TRACE(deviation);
    const FilterGraph graph =
        BlurGraph(fp::GaussianBlur{.stdDeviationX = deviation, .stdDeviationY = deviation});
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

// Wrapping edges never reduce: a reduced texel at the seam would average both edges together.
TEST_F(GeodeReducedResolutionBlurTest, WrappedBlurIsNotReduced) {
  const FilterGraph graph = BlurGraph(fp::GaussianBlur{
      .stdDeviationX = 20, .stdDeviationY = 20, .edgeMode = fp::GaussianBlur::EdgeMode::Wrap});
  geode::GeodeCounters counters;
  const RendererBitmap reduced = render(graph, DrawSquare, true, &counters);
  EXPECT_FALSE(reduced.empty());
  EXPECT_THAT(counters.reducedResolutionBlurs, testing::Eq(0u));
}

// The counters report the filter's fill work: the reduced blur of a 320x320 layer at deviation 16
// (factor 4 on both axes) writes the full-resolution result once and otherwise works on 80x80
// images, far below the two full-resolution box passes it replaces.
TEST_F(GeodeReducedResolutionBlurTest, CountersReportReducedFilterPixels) {
  const FilterGraph graph = BlurGraph(fp::GaussianBlur{.stdDeviationX = 16, .stdDeviationY = 16});
  geode::GeodeCounters reduced;
  geode::GeodeCounters full;
  render(graph, DrawSquare, true, &reduced);
  render(graph, DrawSquare, false, &full);
  constexpr uint64_t kLayer = uint64_t{kViewport} * kViewport;
  constexpr uint64_t kReducedLayer = kLayer / 16;
  EXPECT_THAT(reduced.reducedResolutionBlurs, testing::Eq(1u));
  EXPECT_THAT(full.reducedResolutionBlurs, testing::Eq(0u));
  // Full resolution: two box passes over the layer plus the clip and resolve passes the graph
  // shares with the reduced run.
  const uint64_t shared = full.filterPixels - 2 * kLayer;
  // Reduced: one downsample and two blur passes on the reduced image, and one enlargement.
  EXPECT_THAT(reduced.filterPixels, testing::Eq(shared + 3 * kReducedLayer + kLayer));
  EXPECT_THAT(full.filterPixels, testing::Gt(reduced.filterPixels + kLayer / 2));
}

// Tiled execution reduces each tile on its own grid; the halo covers the reduced kernel, so the
// tiles still agree with the untiled full-resolution blur within the same tolerance.
TEST_F(GeodeReducedResolutionBlurTest, TiledReducedBlurMatchesUntiledFullResolution) {
  const FilterGraph graph = BlurGraph(fp::GaussianBlur{.stdDeviationX = 10, .stdDeviationY = 10});
  std::shared_ptr<geode::GeodeDevice> tiledDevice = geode::GeodeDevice::CreateHeadless();
  ASSERT_THAT(tiledDevice, testing::NotNull());
  tiledDevice->filterEngine().setMaximumTileExtentForTesting(160);
  geode::GeodeCounters counters;
  const RendererBitmap tiled = render(graph, DrawDisc, true, &counters, 1.0, tiledDevice);
  ASSERT_THAT(tiledDevice->filterEngine().lastExecutionMemory().tileExecutions, testing::Gt(1u));
  EXPECT_THAT(counters.reducedResolutionBlurs,
              testing::Eq(tiledDevice->filterEngine().lastExecutionMemory().tileExecutions));
  const RendererBitmap full = render(graph, DrawDisc, false);
  const ImageError error = Compare(tiled, full);
  std::cout << "reduced blur error tiled_sigma10/disc: max " << error.maximum << " mean "
            << error.mean << "\n";
  EXPECT_THAT(error.maximum, testing::Le(kMaximumChannelError));
  EXPECT_THAT(error.mean, testing::Le(kMaximumMeanChannelError));
}

}  // namespace
}  // namespace donner::svg
