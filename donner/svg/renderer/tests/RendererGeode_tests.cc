#include "donner/svg/renderer/RendererGeode.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <future>
#include <latch>
#include <limits>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/FillRule.h"
#include "donner/base/Length.h"
#include "donner/base/MathUtils.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/base/Path.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/css/Color.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/gpu/CommandEncoder.h"
#if defined(__APPLE__)
#include "donner/gpu/metal/MetalDevice.h"
#endif
#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/svg/SVGClipPathElement.h"
#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/SVGImageElement.h"
#include "donner/svg/SVGMarkerElement.h"
#include "donner/svg/SVGPatternElement.h"
#include "donner/svg/components/filter/FilterGraph.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/properties/PaintServer.h"
#include "donner/svg/renderer/PixelFormatUtils.h"  // IWYU pragma: keep - provides UnpremultiplyRgba
#include "donner/svg/renderer/PlacedTextGeometry.h"
#include "donner/svg/renderer/RenderSnapshot.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RendererDriver.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/RendererTinySkia.h"
#include "donner/svg/renderer/RendererUtils.h"
#include "donner/svg/renderer/StrokeParams.h"
#include "donner/svg/renderer/geode/GeodeCheckerboardPipeline.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeFillTolerance.h"
#include "donner/svg/renderer/geode/GeodeFillVariants.h"
#include "donner/svg/renderer/geode/GeodeFilterEngine.h"
#include "donner/svg/renderer/geode/GeodeGpuContext.h"
#include "donner/svg/renderer/geode/GeodePathCacheComponent.h"
#include "donner/svg/renderer/geode/GeodeResidentImageComponent.h"
#include "donner/svg/renderer/geode/GeodeResourceBudget.h"
#include "donner/svg/renderer/geode/tests/GeodeTestContexts.h"
#include "donner/svg/renderer/tests/RgbaTestMatchers.h"
#include "donner/svg/resources/ImageResource.h"
#include "tiny_skia/Pixmap.h"
#include "tiny_skia/filter/Lighting.h"

namespace donner::svg {
namespace {

using test::PixelAt;

constexpr double kViewportSize = 64.0;

using test::Alpha;
using test::IsTransparent;
using test::Near;
using test::Rgba;
using test::RgbaEq;

struct BitmapDiffStats {
  int differingChannels = 0;
  int maxChannelDiff = 0;
  int firstDiffX = -1;
  int firstDiffY = -1;
  int firstDiffChannel = -1;
  uint8_t actualValue = 0;
  uint8_t expectedValue = 0;
};

/// Build a `PaintParams` whose fill is the given solid color.
PaintParams solidFill(const css::RGBA& rgba) {
  PaintParams paint;
  paint.fill = PaintServer::Solid{css::Color(rgba)};
  paint.fillOpacity = 1.0;
  paint.opacity = 1.0;
  return paint;
}

/// Build a `PaintParams` with only a solid stroke (fill=none).
PaintParams solidStroke(const css::RGBA& rgba) {
  PaintParams paint;
  paint.fill = PaintServer::None{};
  paint.stroke = PaintServer::Solid{css::Color(rgba)};
  paint.strokeOpacity = 1.0;
  paint.opacity = 1.0;
  return paint;
}

/// Build a `PaintParams` with both a solid fill and a solid stroke.
PaintParams solidFillAndStroke(const css::RGBA& fill, const css::RGBA& stroke) {
  PaintParams paint;
  paint.fill = PaintServer::Solid{css::Color(fill)};
  paint.stroke = PaintServer::Solid{css::Color(stroke)};
  paint.fillOpacity = 1.0;
  paint.strokeOpacity = 1.0;
  paint.opacity = 1.0;
  return paint;
}

BitmapDiffStats DiffBitmapAgainstStraightRgba(const RendererBitmap& actual,
                                              std::span<const uint8_t> expected, int tolerance = 1,
                                              int inset = 0) {
  BitmapDiffStats stats;
  const int width = actual.dimensions.x;
  const int height = actual.dimensions.y;
  for (int y = inset; y < height - inset; ++y) {
    for (int x = inset; x < width - inset; ++x) {
      const size_t actualOffset =
          static_cast<size_t>(y) * actual.rowBytes + static_cast<size_t>(x) * 4u;
      const size_t expectedOffset =
          (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4u;
      for (int channel = 0; channel < 4; ++channel) {
        const int diff = std::abs(static_cast<int>(actual.pixels[actualOffset + channel]) -
                                  static_cast<int>(expected[expectedOffset + channel]));
        stats.maxChannelDiff = std::max(stats.maxChannelDiff, diff);
        if (diff > tolerance) {
          ++stats.differingChannels;
          if (stats.firstDiffX < 0) {
            stats.firstDiffX = x;
            stats.firstDiffY = y;
            stats.firstDiffChannel = channel;
            stats.actualValue = actual.pixels[actualOffset + channel];
            stats.expectedValue = expected[expectedOffset + channel];
          }
        }
      }
    }
  }

  return stats;
}

double PixelScaleForLighting(const Transform2d& deviceFromFilter) {
  const Vector2d sx = deviceFromFilter.transformPosition(Vector2d(1, 0)) -
                      deviceFromFilter.transformPosition(Vector2d(0, 0));
  const Vector2d sy = deviceFromFilter.transformPosition(Vector2d(0, 1)) -
                      deviceFromFilter.transformPosition(Vector2d(0, 0));
  return std::sqrt((sx.x * sx.x + sx.y * sx.y + sy.x * sy.x + sy.y * sy.y) / 2.0);
}

std::array<double, 6> PixelToUserForLighting(const Transform2d& deviceFromFilter) {
  const Transform2d inv = deviceFromFilter.inverse();
  return {inv.data[0], inv.data[2], inv.data[4], inv.data[1], inv.data[3], inv.data[5]};
}

bool HasShearForLighting(const Transform2d& deviceFromFilter) {
  const double a = deviceFromFilter.data[0];
  const double b = deviceFromFilter.data[1];
  const double c = deviceFromFilter.data[2];
  const double d = deviceFromFilter.data[3];
  const double dot = a * c + b * d;
  const double lenSq1 = a * a + b * b;
  const double lenSq2 = c * c + d * d;
  return dot * dot > 0.0003 * lenSq1 * lenSq2;
}

tiny_skia::filter::LightSourceParams ToTinySkiaLightParams(
    const components::filter_primitive::LightSource& light, const Transform2d& deviceFromFilter) {
  using DonnerLight = components::filter_primitive::LightSource;
  using TinyLight = tiny_skia::filter::LightType;

  tiny_skia::filter::LightSourceParams params;
  switch (light.type) {
    case DonnerLight::Type::Distant: params.type = TinyLight::Distant; break;
    case DonnerLight::Type::Point: params.type = TinyLight::Point; break;
    case DonnerLight::Type::Spot: params.type = TinyLight::Spot; break;
  }

  params.azimuth = light.azimuth;
  params.elevation = light.elevation;

  const Vector2d lightPixel = deviceFromFilter.transformPosition(Vector2d(light.x, light.y));
  const Vector2d pointsAtPixel =
      deviceFromFilter.transformPosition(Vector2d(light.pointsAtX, light.pointsAtY));
  const double pixelScale = PixelScaleForLighting(deviceFromFilter);

  params.x = lightPixel.x;
  params.y = lightPixel.y;
  params.z = light.z * pixelScale;
  params.pointsAtX = pointsAtPixel.x;
  params.pointsAtY = pointsAtPixel.y;
  params.pointsAtZ = light.pointsAtZ * pixelScale;
  params.spotExponent = light.spotExponent;
  params.limitingConeAngle = light.limitingConeAngle;
  params.userX = light.x;
  params.userY = light.y;
  params.userZ = light.z;
  params.userPointsAtX = light.pointsAtX;
  params.userPointsAtY = light.pointsAtY;
  params.userPointsAtZ = light.pointsAtZ;
  return params;
}

std::vector<uint8_t> CpuDiffuseLightingReferenceForFlatSource(
    const components::filter_primitive::DiffuseLighting& primitive,
    const Transform2d& deviceFromFilter) {
  auto src = tiny_skia::Pixmap::fromSize(static_cast<uint32_t>(kViewportSize),
                                         static_cast<uint32_t>(kViewportSize));
  auto dst = tiny_skia::Pixmap::fromSize(static_cast<uint32_t>(kViewportSize),
                                         static_cast<uint32_t>(kViewportSize));
  EXPECT_TRUE(src.has_value());
  EXPECT_TRUE(dst.has_value());
  if (!src.has_value() || !dst.has_value()) {
    return {};
  }

  std::fill(src->data().begin(), src->data().end(), 255u);

  tiny_skia::filter::DiffuseLightingParams params;
  params.surfaceScale = primitive.surfaceScale;
  params.diffuseConstant = primitive.diffuseConstant;
  const css::RGBA rgba = primitive.lightingColor.asRGBA();
  params.lightR = static_cast<double>(rgba.r) / 255.0;
  params.lightG = static_cast<double>(rgba.g) / 255.0;
  params.lightB = static_cast<double>(rgba.b) / 255.0;
  params.pixelToUser = PixelToUserForLighting(deviceFromFilter);
  params.hasShear = HasShearForLighting(deviceFromFilter);
  if (primitive.light.has_value()) {
    params.light = ToTinySkiaLightParams(*primitive.light, deviceFromFilter);
  }

  tiny_skia::filter::diffuseLighting(*src, *dst, params);
  return UnpremultiplyRgba(dst->data());
}

TEST(RendererGeodeDeviceSharing, DefaultRenderersReuseHeadlessDevice) {
  const int before = geode::GeodeDevice::headlessCreationCountForTesting();
  int afterFirst = 0;
  {
    RendererGeode first;
    afterFirst = geode::GeodeDevice::headlessCreationCountForTesting();
  }
  RendererGeode second;

  EXPECT_LE(afterFirst, before + 1);
  EXPECT_EQ(geode::GeodeDevice::headlessCreationCountForTesting(), afterFirst);
}

class RendererGeodeTest : public ::testing::Test {
protected:
  /// Returns a process-wide shared GeodeDevice (created once, destroyed at exit).
  static std::shared_ptr<geode::GeodeDevice> sharedDevice() {
    static auto device = [] {
      return std::shared_ptr<geode::GeodeDevice>(geode::GeodeDevice::CreateHeadless());
    }();
    return device;
  }

  /// Convenience: construct a RendererGeode that shares the test device.
  RendererGeode createRenderer() { return RendererGeode(sharedDevice()); }

  void beginFrame(RendererGeode& renderer) {
    RenderViewport viewport;
    viewport.size = Vector2d(kViewportSize, kViewportSize);
    viewport.devicePixelRatio = 1.0;
    renderer.beginFrame(viewport);
  }

  /**
   * Draws an opaque cyan-blue square through \p context and requires the snapshot to read back
   * as straight, logical RGBA.
   *
   * @param context Context whose target is BGRA.
   */
  void expectBgraTargetSnapshotIsStraightRgba(const std::shared_ptr<geode::GeodeDevice>& context) {
    RendererGeode renderer(context);
    beginFrame(renderer);
    renderer.setPaint(solidFill(css::RGBA(0, 200, 255, 255)));
    renderer.drawRect(Box2d({16, 16}, {48, 48}), StrokeParams{});
    renderer.endFrame();

    const RendererBitmap actual = renderer.takeSnapshot();
    ASSERT_FALSE(actual.empty());
    EXPECT_EQ(actual.alphaType, AlphaType::Unpremultiplied);

    const std::array<uint8_t, 4> center = PixelAt(actual, 32, 32);
    EXPECT_THAT(center, Rgba(testing::Le(2), Near(200, 2), Near(255, 2), testing::Eq(255)))
        << "BGRA readback must be converted back to logical RGBA";
  }

  /// Take the finished frame's target texture, so the checkerboard pass can draw onto content the
  /// renderer already composed and the result can be read back.
  std::shared_ptr<const RendererTextureSnapshot> takeFinishedFrame(RendererGeode& renderer) {
    return renderer.takeTextureSnapshot();
  }

  /// Draw one checkerboard pass over a finished frame through the shared Geode helper.
  bool drawCheckerboard(const RendererTextureSnapshot& frame,
                        const geode::CheckerboardUnderlayParams& params,
                        geode::GeodeCheckerboardPipeline::BlendMode blendMode =
                            geode::GeodeCheckerboardPipeline::BlendMode::DestinationOver) {
    const auto& geodeFrame = static_cast<const RendererGeodeTextureSnapshot&>(frame);
    const gpu::Texture* target = geodeFrame.runtimeTexture();
    return target != nullptr && checkerboardPass_.draw(*sharedDevice(), *target,
                                                       geodeFrame.dimensions(), params, blendMode);
  }

  RendererBitmap renderFlatSourceThroughFilter(const components::FilterGraph& graph,
                                               const Transform2d& deviceFromFilter) {
    RendererGeode renderer = createRenderer();
    beginFrame(renderer);
    renderer.setTransform(deviceFromFilter);
    renderer.pushFilterLayer(graph, Box2d({0, 0}, {kViewportSize, kViewportSize}));

    renderer.setTransform(Transform2d());
    renderer.setPaint(solidFill(css::RGBA(255, 255, 255, 255)));
    renderer.drawRect(Box2d({-4, -4}, {kViewportSize + 4, kViewportSize + 4}), StrokeParams{});

    renderer.popFilterLayer();
    renderer.endFrame();
    return renderer.takeSnapshot();
  }

  RendererBitmap drawPreparedEntityRange(RendererGeode& renderer, SVGDocument& document,
                                         Entity entity) {
    ParseWarningSink warningSink;
    RendererUtils::prepareDocumentForRendering(document, /*verbose=*/false, warningSink);

    RenderViewport viewport;
    viewport.size = Vector2d(100.0, 100.0);
    viewport.devicePixelRatio = 1.0;
    RendererDriver driver(renderer);
    driver.drawEntityRange(document.registry(), entity, entity, viewport, Transform2d());
    return renderer.takeSnapshot();
  }

  /// One pass per fixture, matching the per-consumer ownership the helper documents.
  geode::GeodeCheckerboardPass checkerboardPass_;
};

// ----------------------------------------------------------------------------

TEST_F(RendererGeodeTest, TransformedLocalFilterAdmissionCoversItsTileWork) {
  using namespace components;
  constexpr uint32_t kWidth = 1024;
  constexpr uint32_t kHeight = 768;
  for (bool changePreference : {false, true}) {
    SCOPED_TRACE(changePreference);
    std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
    ASSERT_TRUE(device);
    RendererGeode renderer(device);
    RenderViewport viewport;
    viewport.size = Vector2d(kWidth, kHeight);
    renderer.beginFrame(viewport);
    FilterGraph graph;
    FilterNode blur;
    blur.primitive = filter_primitive::GaussianBlur{.stdDeviationX = 4, .stdDeviationY = 2};
    graph.nodes.push_back(blur);
    renderer.setTransform(Transform2d::SkewX(0.2));
    renderer.pushFilterLayer(graph, Box2d({0, 0}, {960, 640}));
    const auto admitted = renderer.resourceStats();
    ASSERT_FALSE(admitted.filterBudgetRejected);
    if (changePreference) {
      device->filterEngine().setMaximumTileExtentForTesting(16);
    }
    renderer.setTransform(Transform2d());
    renderer.setPaint(solidFill(css::RGBA(255, 255, 255, 255)));
    renderer.drawRect(Box2d({0, 0}, {kWidth, kHeight}), StrokeParams{});
    renderer.popFilterLayer();
    const auto observed = device->filterEngine().lastExecutionMemory();
    ASSERT_GT(observed.tileExecutions, 1u);
    uint64_t untiledWork = 0;
    uint64_t untiledBytes = 0;
    ASSERT_TRUE(FilterGraphExecutionCost(graph, uint64_t{kWidth} * kHeight,
                                         FilterMemoryModel::GpuAllNodes, untiledWork,
                                         untiledBytes));
    EXPECT_GT(observed.workUnits, untiledWork);
    EXPECT_GE(admitted.filterWorkUnits, observed.workUnits);
    EXPECT_GE(admitted.filterRetainedBytes, observed.total());
    renderer.endFrame();
    EXPECT_FALSE(renderer.resourceStats().filterBudgetRejected);
    EXPECT_FALSE(renderer.resourceStats().surfaceBudgetRejected);
    EXPECT_FALSE(renderer.takeSnapshot().empty());
  }
}

// A frame whose filters outgrow the retained GPU memory submits a chunk and retries the filter in
// it. A frame recovered that way drew every filter at full resolution, so it reports no refusal.
// These blurs outgrow the memory every few filters, and the frame stops short of its work limit.
// Natively the surface budget asks for each chunk before the filter budget refuses anything; the
// filter execution budget tests cover a chunk that follows a memory refusal.
TEST_F(RendererGeodeTest, FilterFrameRecoveredThroughAMemoryChunkReportsNoRefusal) {
  using namespace components;
  constexpr uint32_t kWidth = 1024;
  constexpr uint32_t kHeight = 768;
  constexpr uint64_t kFilters = 24;
  std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
  ASSERT_TRUE(device);
  RendererGeode renderer(device);
  RenderViewport viewport;
  viewport.size = Vector2d(kWidth, kHeight);
  renderer.beginFrame(viewport);
  FilterGraph graph;
  FilterNode blur;
  blur.primitive = filter_primitive::GaussianBlur{.stdDeviationX = 2, .stdDeviationY = 2};
  graph.nodes.push_back(blur);
  for (uint64_t i = 0; i < kFilters; ++i) {
    renderer.pushFilterLayer(graph, Box2d({0, 0}, {kWidth, kHeight}));
    renderer.setPaint(solidFill(css::RGBA(255, 255, 255, 255)));
    renderer.drawRect(Box2d({0, 0}, {kWidth, kHeight}), StrokeParams{});
    renderer.popFilterLayer();
  }
  const RendererResourceStats stats = renderer.resourceStats();
  renderer.endFrame();
  ASSERT_GT(renderer.filterBudgetChunksForTesting(), 0u)
      << "the frame must outgrow one chunk's memory for this to cover the retry";
  EXPECT_EQ(stats.filterExecutions, kFilters) << "every filter ran";
  EXPECT_EQ(stats.filterGraphsRefused, 0u);
  EXPECT_FALSE(stats.filterBudgetRejected);
  EXPECT_EQ(stats.filterReducedRasters, 0u)
      << "a chunk lifts a memory refusal at full resolution, so no filter runs reduced";
}

// Tiles on the raster edge keep the transparent outside of the raster, so the box passes there
// must keep their spill past the edge exactly as an untiled raster with room does. Covers even and
// odd windows, an anisotropic blur whose X axis uses the sampled kernel, and a drop shadow. Its
// offset is zero: blurring first, Geode would need content beyond the raster for an offset to
// pull back, which is the capture margin's job, not the blur's.
TEST_F(RendererGeodeTest, TiledBlurNearRasterEdgeMatchesBlurWithRoom) {
  using namespace components;
  const std::array<std::pair<std::string_view, FilterPrimitive>, 4> primitives{{
      {"odd_window", filter_primitive::GaussianBlur{.stdDeviationX = 6, .stdDeviationY = 6}},
      {"even_window", filter_primitive::GaussianBlur{.stdDeviationX = 5.5, .stdDeviationY = 5.5}},
      {"anisotropic", filter_primitive::GaussianBlur{.stdDeviationX = 1.5, .stdDeviationY = 6}},
      {"drop_shadow",
       filter_primitive::DropShadow{.dx = 0,
                                    .dy = 0,
                                    .stdDeviationX = 6,
                                    .stdDeviationY = 6,
                                    .floodColor = css::Color(css::RGBA(0, 0, 255, 255))}},
  }};
  for (const auto& [label, primitive] : primitives) {
    SCOPED_TRACE(label);
    FilterGraph graph;
    FilterNode node;
    node.primitive = primitive;
    graph.nodes.push_back(node);
    const auto render = [&](const std::shared_ptr<geode::GeodeDevice>& device, int size,
                            double origin) {
      RendererGeode renderer(device);
      RenderViewport viewport;
      viewport.size = Vector2d(size, size);
      viewport.devicePixelRatio = 1.0;
      renderer.beginFrame(viewport);
      renderer.pushFilterLayer(graph, Box2d({0, 0}, {size, size}));
      renderer.setPaint(solidFill(css::RGBA(46, 139, 87, 255)));
      renderer.drawRect(Box2d({origin + 2, origin + 2}, {origin + 62, origin + 62}),
                        StrokeParams{});
      renderer.popFilterLayer();
      renderer.endFrame();
      return renderer.takeSnapshot();
    };

    std::shared_ptr<geode::GeodeDevice> tiledDevice = geode::GeodeDevice::CreateHeadless();
    ASSERT_THAT(tiledDevice, testing::NotNull());
    tiledDevice->filterEngine().setMaximumTileExtentForTesting(48);
    const RendererBitmap atEdge = render(tiledDevice, 64, 0);
    ASSERT_THAT(tiledDevice->filterEngine().lastExecutionMemory().tileExecutions, testing::Gt(1u));
    const RendererBitmap withRoom = render(sharedDevice(), 128, 32);
    ASSERT_FALSE(atEdge.empty());
    ASSERT_FALSE(withRoom.empty());
    editor::tests::CompareBitmapToBitmap(
        atEdge, editor::tests::CropBitmap(withRoom, Vector2i(32, 32), Vector2i(64, 64)),
        "tiled_blur_raster_edge_spill_" + std::string(label),
        editor::tests::PixelmatchIdentityParams());
  }
}

// A kernel wider than 25 taps still widens the tile halo, so splitting the raster into tiles
// leaves the result unchanged. The 27-tap rows need a 27-pixel halo on each side, and 64-pixel
// tiles split the 200-pixel raster into columns 10 pixels wide.
TEST_F(RendererGeodeTest, TiledLargeConvolveMatchesUntiledConvolve) {
  using namespace components;
  constexpr int kOrderX = 27;
  constexpr int kOrderY = 3;
  constexpr int kWidth = 200;
  constexpr int kHeight = 48;
  filter_primitive::ConvolveMatrix convolve{.orderX = kOrderX, .orderY = kOrderY};
  for (int i = 0; i < kOrderX * kOrderY; ++i) {
    // Integers from -1 to 3 that sum to a positive divisor and vary along both axes.
    convolve.kernelMatrix.push_back(static_cast<double>((i * 3 + i / kOrderX) % 5) - 1.0);
  }
  convolve.edgeMode = filter_primitive::ConvolveMatrix::EdgeMode::None;
  FilterGraph graph;
  FilterNode node;
  node.primitive = convolve;
  graph.nodes.push_back(node);
  const auto render = [&](const std::shared_ptr<geode::GeodeDevice>& device) {
    RendererGeode renderer(device);
    RenderViewport viewport;
    viewport.size = Vector2d(kWidth, kHeight);
    viewport.devicePixelRatio = 1.0;
    renderer.beginFrame(viewport);
    renderer.pushFilterLayer(graph, Box2d({0, 0}, {kWidth, kHeight}));
    renderer.setPaint(solidFill(css::RGBA(46, 139, 87, 255)));
    renderer.drawRect(Box2d({20, 8}, {110, 40}), StrokeParams{});
    renderer.setPaint(solidFill(css::RGBA(220, 20, 60, 153)));
    renderer.drawRect(Box2d({100, 2}, {180, 30}), StrokeParams{});
    renderer.popFilterLayer();
    renderer.endFrame();
    return renderer.takeSnapshot();
  };

  std::shared_ptr<geode::GeodeDevice> tiledDevice = geode::GeodeDevice::CreateHeadless();
  ASSERT_THAT(tiledDevice, testing::NotNull());
  tiledDevice->filterEngine().setMaximumTileExtentForTesting(64);
  const RendererBitmap tiled = render(tiledDevice);
  ASSERT_THAT(tiledDevice->filterEngine().lastExecutionMemory().tileExecutions, testing::Gt(1u));
  const RendererBitmap untiled = render(sharedDevice());
  ASSERT_THAT(sharedDevice()->filterEngine().lastExecutionMemory().tileExecutions, testing::Eq(1u));
  ASSERT_FALSE(untiled.empty());
  // Premise: the kernel applied. Its whole footprint around this point lies inside the opaque
  // square, so the result there is opaque rather than the transparent black of a refused kernel.
  ASSERT_EQ(PixelAt(untiled, 65, 24)[3], 255u);
  editor::tests::CompareBitmapToBitmap(tiled, untiled, "tiled_large_convolve",
                                       editor::tests::PixelmatchIdentityParams());
}

// Under a skew the filter runs on a local raster aligned to the filter region and resampled from
// the device capture. Moving the content by whole device pixels must move the result by the same
// pixels and change nothing else, so the resample's weights cannot depend on where the capture
// sits.
TEST_F(RendererGeodeTest, SkewedLocalRasterFilterIsUnchangedByWholePixelMoves) {
  using namespace components;
  FilterGraph graph;
  FilterNode node;
  node.primitive = filter_primitive::GaussianBlur{.stdDeviationX = 0.01, .stdDeviationY = 0.01};
  graph.nodes.push_back(node);
  const auto render = [&](double moveX) {
    RendererGeode renderer(sharedDevice());
    RenderViewport viewport;
    viewport.size = Vector2d(128, 64);
    viewport.devicePixelRatio = 1.0;
    renderer.beginFrame(viewport);
    Transform2d skewedFromUser;
    skewedFromUser.data[2] = 1.0;
    renderer.setTransform(skewedFromUser * Transform2d::Translate(moveX, 0));
    renderer.pushFilterLayer(graph, Box2d({-100, 0}, {100, 64}));
    renderer.setPaint(solidFill(css::RGBA(0, 255, 0, 255)));
    renderer.drawRect(Box2d({4, 8}, {14, 30}), StrokeParams{});
    renderer.popFilterLayer();
    renderer.endFrame();
    return renderer.takeSnapshot();
  };
  // Placements at the canvas origin and with room on both sides: the resample into the local
  // raster and the composite back to the device each varied with some of them.
  constexpr int kBase = 32;
  const RendererBitmap base = render(kBase);
  ASSERT_FALSE(base.empty());
  const RendererBitmap baseCrop =
      editor::tests::CropBitmap(base, Vector2i(kBase, 0), Vector2i(63, 64));
  for (const int at : {0, kBase + 1, 2 * kBase}) {
    SCOPED_TRACE(at);
    const RendererBitmap moved = render(at);
    ASSERT_FALSE(moved.empty());
    editor::tests::CompareBitmapToBitmap(
        editor::tests::CropBitmap(moved, Vector2i(at, 0), Vector2i(63, 64)), baseCrop,
        "skewed_local_raster_at_" + std::to_string(at), editor::tests::PixelmatchIdentityParams());
  }
}

// A tiled execution copies each tile of the backdrop alongside the source graphic, so it must read
// the same BackgroundImage and BackgroundAlpha as one execution over the whole raster.
TEST_F(RendererGeodeTest, TiledFilterReadsTheSameBackdropAsUntiled) {
  using namespace components;
  for (const FilterStandardInput input :
       {FilterStandardInput::BackgroundImage, FilterStandardInput::BackgroundAlpha}) {
    SCOPED_TRACE(static_cast<int>(input));
    FilterGraph graph;
    FilterNode offset;
    offset.primitive = filter_primitive::Offset{.dx = 24, .dy = 8};
    offset.inputs = {input};
    graph.nodes.push_back(offset);
    FilterNode blur;
    blur.primitive = filter_primitive::GaussianBlur{.stdDeviationX = 2, .stdDeviationY = 2};
    graph.nodes.push_back(blur);
    const auto render = [&](const std::shared_ptr<geode::GeodeDevice>& device) {
      RendererGeode renderer(device);
      RenderViewport viewport;
      viewport.size = Vector2d(192, 192);
      viewport.devicePixelRatio = 1.0;
      renderer.beginFrame(viewport);
      renderer.setPaint(solidFill(css::RGBA(46, 139, 87, 255)));
      renderer.drawRect(Box2d({8, 8}, {88, 120}), StrokeParams{});
      renderer.pushFilterLayer(graph, Box2d({0, 0}, {192, 192}));
      renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
      renderer.drawRect(Box2d({150, 150}, {180, 180}), StrokeParams{});
      renderer.popFilterLayer();
      renderer.endFrame();
      return renderer.takeSnapshot();
    };

    std::shared_ptr<geode::GeodeDevice> tiledDevice = geode::GeodeDevice::CreateHeadless();
    ASSERT_THAT(tiledDevice, testing::NotNull());
    tiledDevice->filterEngine().setMaximumTileExtentForTesting(96);
    const RendererBitmap tiled = render(tiledDevice);
    ASSERT_THAT(tiledDevice->filterEngine().lastExecutionMemory().tileExecutions, testing::Gt(1u));
    const RendererBitmap whole = render(sharedDevice());
    ASSERT_THAT(sharedDevice()->filterEngine().lastExecutionMemory().tileExecutions,
                testing::Eq(1u));
    ASSERT_FALSE(tiled.empty());
    ASSERT_FALSE(whole.empty());
    const auto alphaAt = [&whole](int x, int y) {
      return whole.pixels[static_cast<std::size_t>(y) * whole.rowBytes +
                          static_cast<std::size_t>(x) * 4u + 3u];
    };
    // The moved backdrop covers (60, 60); the filtered red square at (165, 165) is replaced.
    EXPECT_THAT(alphaAt(60, 60), testing::Eq(255));
    EXPECT_THAT(alphaAt(165, 165), testing::Eq(0));
    editor::tests::CompareBitmapToBitmap(tiled, whole, "tiled_backdrop",
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST_F(RendererGeodeTest, TransformedFilterRestoreFailureAbandonsTheFrame) {
  using namespace components;
  std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
  ASSERT_THAT(device, testing::NotNull());
  RendererGeode renderer(device);
  beginFrame(renderer);
  renderer.pushIsolatedLayer(0.8, MixBlendMode::Normal);
  FilterGraph graph;
  FilterNode blur;
  blur.primitive = filter_primitive::GaussianBlur{.stdDeviationX = 4, .stdDeviationY = 2};
  graph.nodes.push_back(blur);
  renderer.setTransform(Transform2d::SkewX(0.2));
  renderer.pushFilterLayer(graph, Box2d({0, 0}, {kViewportSize, kViewportSize}));
  ASSERT_GT(renderer.resourceStats().filterRetainedBytes, 0u);
  renderer.setTransform(Transform2d());
  renderer.setPaint(solidFill(css::RGBA(255, 255, 255, 255)));
  renderer.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});
  renderer.injectFilterFrameSuspensionAndRestoreFailureForTesting();
  ASSERT_THAT(device->counters(), testing::NotNull());
  const uint64_t submitsBefore = device->counters()->submits;

  renderer.popFilterLayer();

  EXPECT_THAT(renderer.deviceLost(), testing::IsTrue());
  EXPECT_THAT(renderer.hasActiveDrawingEncoderForTesting(), testing::IsFalse());
  EXPECT_THAT(renderer.filterFrameFailureInjectionPendingForTesting(), testing::IsFalse());
  EXPECT_THAT(device->counters()->submits, testing::Eq(submitsBefore));
  EXPECT_THAT(renderer.failedFilterTextureCountForTesting(), testing::Gt(0u));
  const size_t retainedAfterFilter = renderer.failedFilterTextureCountForTesting();
  renderer.popIsolatedLayer();
  EXPECT_THAT(renderer.hasActiveDrawingEncoderForTesting(), testing::IsFalse());
  EXPECT_THAT(renderer.failedFilterTextureCountForTesting(), testing::Gt(retainedAfterFilter));
  EXPECT_THAT(device->counters()->submits, testing::Eq(submitsBefore));
  const size_t retainedAfterUnwind = renderer.failedFilterTextureCountForTesting();
  EXPECT_THAT(renderer.beginPatternTile(Box2d({0, 0}, {32, 32}), Transform2d()),
              testing::IsFalse());
  EXPECT_THAT(renderer.failedFilterTextureCountForTesting(), testing::Eq(retainedAfterUnwind));
  EXPECT_THAT(device->counters()->submits, testing::Eq(submitsBefore));
  renderer.endFrame();
  EXPECT_THAT(renderer.takeSnapshot().empty(), testing::IsTrue());

  const size_t retainedAfterFailedFrame = renderer.failedFilterTextureCountForTesting();
  beginFrame(renderer);
  EXPECT_THAT(renderer.hasActiveDrawingEncoderForTesting(), testing::IsFalse());
  renderer.drawRect(Box2d({0, 0}, {32, 32}), StrokeParams{});
  EXPECT_THAT(renderer.beginPatternTile(Box2d({0, 0}, {32, 32}), Transform2d()),
              testing::IsFalse());
  renderer.endFrame();
  EXPECT_THAT(renderer.deviceLost(), testing::IsTrue());
  EXPECT_THAT(renderer.takeSnapshot().empty(), testing::IsTrue());
  EXPECT_THAT(renderer.failedFilterTextureCountForTesting(), testing::Eq(retainedAfterFailedFrame));
  EXPECT_THAT(device->counters()->textureCreates, testing::Eq(0u));
  EXPECT_THAT(device->counters()->drawCalls, testing::Eq(0u));
  EXPECT_THAT(device->counters()->submits, testing::Eq(0u));
}

TEST_F(RendererGeodeTest, TransformedShadowMergeAbandonsWhenItsEncoderCannotClose) {
  using namespace components;
  std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
  ASSERT_THAT(device, testing::NotNull());
  RendererGeode renderer(device);
  beginFrame(renderer);
  FilterGraph graph;
  graph.nodes = {FilterNode{.primitive = filter_primitive::DropShadow{
                                .dx = 10, .dy = 6, .stdDeviationX = 3, .stdDeviationY = 2}}};
  renderer.setTransform(Transform2d::SkewX(0.2));
  renderer.pushFilterLayer(graph, Box2d({0, 0}, {kViewportSize, kViewportSize}));
  renderer.setPaint(solidFill(css::RGBA(255, 255, 255, 255)));
  renderer.drawRect(Box2d({8, 8}, {38, 40}), StrokeParams{});
  ASSERT_THAT(device->counters(), testing::NotNull());
  const uint64_t submitsBefore = device->counters()->submits;
  // The local shadow pass closes successfully. Fail the close of the projection pass.
  renderer.injectFrameEncoderCloseFailureForTesting(/*successfulCloses=*/1);
  renderer.popFilterLayer();

  EXPECT_THAT(renderer.deviceLost(), testing::IsTrue());
  EXPECT_THAT(renderer.hasActiveDrawingEncoderForTesting(), testing::IsFalse());
  EXPECT_THAT(renderer.failedFilterTextureCountForTesting(), testing::Gt(0u));
  renderer.endFrame();
  EXPECT_THAT(device->counters()->submits, testing::Eq(submitsBefore));
  EXPECT_TRUE(renderer.takeSnapshot().empty());
}

TEST_F(RendererGeodeTest, TransformedResampleArenaRefusalSkipsFilterAndRecovers) {
  using namespace components;
  std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
  ASSERT_THAT(device, testing::NotNull());
  RendererGeode renderer(device);
  beginFrame(renderer);
  FilterGraph graph;
  FilterNode blur;
  blur.primitive = filter_primitive::GaussianBlur{.stdDeviationX = 4, .stdDeviationY = 2};
  graph.nodes.push_back(blur);
  renderer.setTransform(Transform2d::SkewX(0.2));
  renderer.pushFilterLayer(graph, Box2d({0, 0}, {kViewportSize, kViewportSize}));
  renderer.setTransform(Transform2d());
  renderer.setPaint(solidFill(css::RGBA(255, 255, 255, 255)));
  renderer.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});
  renderer.injectTransformedResampleArenaWriteFailureForTesting();
  ASSERT_THAT(device->counters(), testing::NotNull());
  const uint64_t submitsBefore = device->counters()->submits;

  renderer.popFilterLayer();
  EXPECT_THAT(renderer.deviceLost(), testing::IsFalse());
  EXPECT_THAT(renderer.hasActiveDrawingEncoderForTesting(), testing::IsFalse());
  EXPECT_THAT(device->counters()->submits, testing::Eq(submitsBefore));
  EXPECT_THAT(renderer.failedFilterTextureCountForTesting(), testing::Eq(0u));
  EXPECT_THAT(renderer.recoverableArenaTextureCountForTesting(), testing::Gt(0u));
  EXPECT_THAT(renderer.takeSnapshot().empty(), testing::IsTrue());
  renderer.endFrame();
  EXPECT_THAT(device->counters()->submits, testing::Eq(submitsBefore));
  EXPECT_THAT(renderer.recoverableArenaTextureCountForTesting(), testing::Eq(0u));

  beginFrame(renderer);
  renderer.setPaint(solidFill(css::RGBA(0, 255, 0, 255)));
  renderer.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});
  renderer.endFrame();
  const RendererBitmap recovered = renderer.takeSnapshot();
  ASSERT_THAT(recovered.empty(), testing::IsFalse());
  EXPECT_THAT(PixelAt(recovered, 32, 32), RgbaEq(0, 255, 0, 255));
}

TEST_F(RendererGeodeTest, DiscardedFailedRendererRetiresRecoverableTextures) {
  std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
  ASSERT_THAT(device, testing::NotNull());
  {
    RendererGeode offscreen(device);
    beginFrame(offscreen);
    components::FilterGraph graph;
    components::FilterNode blur;
    blur.primitive =
        components::filter_primitive::GaussianBlur{.stdDeviationX = 4, .stdDeviationY = 2};
    graph.nodes.push_back(blur);
    offscreen.setTransform(Transform2d::SkewX(0.2));
    offscreen.pushFilterLayer(graph, Box2d({0, 0}, {kViewportSize, kViewportSize}));
    offscreen.setTransform(Transform2d());
    offscreen.setPaint(solidFill(css::RGBA(255, 255, 255, 255)));
    offscreen.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});
    offscreen.injectTransformedResampleArenaWriteFailureForTesting();
    offscreen.popFilterLayer();
    EXPECT_THAT(offscreen.failedFilterTextureCountForTesting(), testing::Eq(0u));
    EXPECT_THAT(device->pendingFencedTextureBackings(), testing::Gt(0u));
  }
  EXPECT_THAT(device->pendingFencedTextureBackings(), testing::Eq(0u));

  RendererGeode sibling(device);
  beginFrame(sibling);
  sibling.setPaint(solidFill(css::RGBA(0, 255, 0, 255)));
  sibling.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});
  sibling.endFrame();
  const RendererBitmap recovered = sibling.takeSnapshot();
  ASSERT_THAT(recovered.empty(), testing::IsFalse());
  EXPECT_THAT(PixelAt(recovered, 32, 32), RgbaEq(0, 255, 0, 255));
}

TEST_F(RendererGeodeTest, TransformedFilterFailureUnwindsAnOpenPatternTile) {
  using namespace components;
  std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
  ASSERT_THAT(device, testing::NotNull());
  RendererGeode renderer(device);
  beginFrame(renderer);
  ASSERT_THAT(renderer.beginPatternTile(Box2d({0, 0}, {32, 32}), Transform2d()), testing::IsTrue());
  FilterGraph graph;
  FilterNode blur;
  blur.primitive = filter_primitive::GaussianBlur{.stdDeviationX = 4, .stdDeviationY = 2};
  graph.nodes.push_back(blur);
  renderer.setTransform(Transform2d::SkewX(0.2));
  renderer.pushFilterLayer(graph, Box2d({0, 0}, {32, 32}));
  renderer.setTransform(Transform2d());
  renderer.setPaint(solidFill(css::RGBA(255, 255, 255, 255)));
  renderer.drawRect(Box2d({0, 0}, {32, 32}), StrokeParams{});
  renderer.injectFilterFrameSuspensionAndRestoreFailureForTesting();
  ASSERT_THAT(device->counters(), testing::NotNull());
  const uint64_t submitsBefore = device->counters()->submits;

  renderer.popFilterLayer();
  const size_t retainedAfterFilter = renderer.failedFilterTextureCountForTesting();
  renderer.endPatternTile(/*forStroke=*/false, std::nullopt);

  EXPECT_THAT(renderer.deviceLost(), testing::IsTrue());
  EXPECT_THAT(renderer.hasActiveDrawingEncoderForTesting(), testing::IsFalse());
  EXPECT_THAT(renderer.failedFilterTextureCountForTesting(), testing::Gt(retainedAfterFilter));
  EXPECT_THAT(device->counters()->submits, testing::Eq(submitsBefore));
  renderer.endFrame();
  EXPECT_THAT(renderer.takeSnapshot().empty(), testing::IsTrue());
}

components::FilterGraph MultiChunkMorphologyGraph() {
  using namespace components;
  FilterGraph graph;
  graph.colorInterpolationFilters = svg::ColorInterpolationFilters::SRGB;
  for (size_t index = 0; index != 5; ++index) {
    FilterNode node;
    node.primitive = filter_primitive::Morphology{
        .op = filter_primitive::Morphology::Operator::Dilate, .radiusX = 256, .radiusY = 256};
    graph.nodes.push_back(std::move(node));
  }
  return graph;
}

TEST_F(RendererGeodeTest, AcceptedFilterChunkLossAbandonsOrdinaryFrame) {
  std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
  ASSERT_THAT(device, testing::NotNull());
  RendererGeode renderer(device);
  beginFrame(renderer);
  const components::FilterGraph graph = MultiChunkMorphologyGraph();
  renderer.pushFilterLayer(graph, Box2d({0, 0}, {kViewportSize, kViewportSize}));
  renderer.setPaint(solidFill(css::RGBA(255, 255, 255, 255)));
  renderer.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});
  size_t acceptedChunks = 0;
  device->filterEngine().setChunkSubmittedHookForTesting([&](size_t chunk) {
    acceptedChunks = chunk;
    if (chunk == 1) {
      device->markDeviceLost("injected ordinary filter chunk loss");
    }
  });
  const uint64_t submitsBeforeBoundary = device->counters()->submits;

  renderer.popFilterLayer();

  EXPECT_THAT(acceptedChunks, testing::Eq(1u));
  EXPECT_THAT(device->counters()->submits, testing::Eq(submitsBeforeBoundary))
      << "a chunk boundary inside a frame closes a command buffer, so losing the device there "
         "must leave the frame's work unsubmitted";
  EXPECT_THAT(device->counters()->commandBuffers, testing::Eq(0u));
  EXPECT_THAT(renderer.deviceLost(), testing::IsTrue());
  EXPECT_THAT(renderer.hasActiveDrawingEncoderForTesting(), testing::IsFalse());
  EXPECT_THAT(renderer.failedFilterTextureCountForTesting(), testing::Gt(0u));
  const size_t retainedAfterLoss = renderer.failedFilterTextureCountForTesting();
  const uint64_t submitsAfterLoss = device->counters()->submits;
  renderer.pushFilterLayer(graph, Box2d({0, 0}, {kViewportSize, kViewportSize}));
  renderer.popFilterLayer();
  EXPECT_THAT(renderer.failedFilterTextureCountForTesting(), testing::Eq(retainedAfterLoss));
  EXPECT_THAT(device->counters()->submits, testing::Eq(submitsAfterLoss));
  renderer.endFrame();
  EXPECT_THAT(renderer.takeSnapshot().empty(), testing::IsTrue());
}

TEST_F(RendererGeodeTest, AcceptedFilterChunkLossAbandonsTransformedFrame) {
  std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
  ASSERT_THAT(device, testing::NotNull());
  RendererGeode renderer(device);
  beginFrame(renderer);
  const components::FilterGraph graph = MultiChunkMorphologyGraph();
  renderer.setTransform(Transform2d::SkewX(0.2));
  renderer.pushFilterLayer(graph, Box2d({0, 0}, {kViewportSize, kViewportSize}));
  renderer.setTransform(Transform2d());
  renderer.setPaint(solidFill(css::RGBA(255, 255, 255, 255)));
  renderer.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});
  size_t acceptedChunks = 0;
  device->filterEngine().setChunkSubmittedHookForTesting([&](size_t chunk) {
    acceptedChunks = chunk;
    if (chunk == 1) {
      device->markDeviceLost("injected transformed filter chunk loss");
    }
  });
  const uint64_t submitsBeforeBoundary = device->counters()->submits;

  renderer.popFilterLayer();

  EXPECT_THAT(acceptedChunks, testing::Eq(1u));
  EXPECT_THAT(device->counters()->submits, testing::Eq(submitsBeforeBoundary))
      << "a chunk boundary inside a frame closes a command buffer, so losing the device there "
         "must leave the frame's work unsubmitted";
  EXPECT_THAT(device->counters()->commandBuffers, testing::Eq(0u));
  EXPECT_THAT(renderer.deviceLost(), testing::IsTrue());
  EXPECT_THAT(renderer.hasActiveDrawingEncoderForTesting(), testing::IsFalse());
  EXPECT_THAT(renderer.failedFilterTextureCountForTesting(), testing::Gt(0u));
  const size_t retainedAfterLoss = renderer.failedFilterTextureCountForTesting();
  const uint64_t submitsAfterLoss = device->counters()->submits;
  EXPECT_THAT(renderer.beginPatternTile(Box2d({0, 0}, {32, 32}), Transform2d()),
              testing::IsFalse());
  EXPECT_THAT(renderer.failedFilterTextureCountForTesting(), testing::Eq(retainedAfterLoss));
  EXPECT_THAT(device->counters()->submits, testing::Eq(submitsAfterLoss));
  renderer.endFrame();
  EXPECT_THAT(renderer.takeSnapshot().empty(), testing::IsTrue());
}

TEST_F(RendererGeodeTest, FilterChunkBoundaryInsideAFrameClosesBuffersWithoutSubmittingOrWaiting) {
  std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
  ASSERT_THAT(device, testing::NotNull());
  RendererGeode renderer(device);
  beginFrame(renderer);
  ASSERT_THAT(device->counters(), testing::NotNull());
  const geode::GeodeCounters* counters = device->counters();
  // Armed for one wait: the completion wait a chunk boundary used to force would consume it and
  // report a hang, which is terminal. A boundary inside one submission may not take that wait,
  // because the passes on both sides of it belong to that submission and are ordered by it.
  device->setQueueWaitResultForTesting(geode::GpuWaitResult::TimedOut);

  renderer.pushFilterLayer(MultiChunkMorphologyGraph(),
                           Box2d({0, 0}, {kViewportSize, kViewportSize}));
  renderer.setPaint(solidFill(css::RGBA(255, 255, 255, 255)));
  renderer.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});
  renderer.popFilterLayer();

  EXPECT_THAT(renderer.deviceLost(), testing::IsFalse())
      << "crossing the per-command-buffer pass bound inside a frame must not wait on the queue";
  EXPECT_THAT(counters->submits, testing::Eq(0u))
      << "crossing that bound must close a command buffer, not submit one";
  device->setQueueWaitResultForTesting(std::nullopt);

  renderer.endFrame();

  EXPECT_THAT(renderer.deviceLost(), testing::IsFalse());
  EXPECT_THAT(counters->submits, testing::Eq(1u));
  EXPECT_THAT(counters->commandBuffers, testing::Ge(3u))
      << "the graph's passes must still be spread across several command buffers";
}

/// Draws a filter whose chunks pass the most command buffers one submission carries, so the
/// frame has to split. The graph is ordinary; what multiplies its passes is the small working
/// tile the caller sets on the engine, which keeps the document (and the case) small.
/// @param renderer Renderer with an open frame, on a device whose maximum tile extent is small.
void DrawFilterGraphPastTheSubmissionBound(RendererGeode& renderer) {
  using namespace components;
  FilterGraph graph;
  graph.primitiveUnits = PrimitiveUnits::ObjectBoundingBox;
  graph.elementBoundingBox = Box2d({0, 0}, Vector2d(1.1, 1.1));
  FilterNode flood;
  flood.primitive =
      filter_primitive::Flood{.floodColor = css::Color(css::RGBA(255, 255, 255, 255))};
  graph.nodes.push_back(flood);
  FilterNode blur;
  blur.primitive = filter_primitive::GaussianBlur{.stdDeviationX = 2.0, .stdDeviationY = 2.0};
  graph.nodes.push_back(blur);
  renderer.setTransform(Transform2d::Scale(1.1));
  renderer.pushFilterLayer(graph, Box2d({0, 0}, {kViewportSize, kViewportSize}));
  renderer.setTransform(Transform2d());
  renderer.setPaint(solidFill(css::RGBA(255, 255, 255, 255)));
  renderer.drawRect(Box2d({-4, -4}, {kViewportSize + 4, kViewportSize + 4}), StrokeParams{});
  renderer.popFilterLayer();
}

TEST_F(RendererGeodeTest, AFramePastTheSubmissionBoundSplitsAndStillRenders) {
  std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
  ASSERT_THAT(device, testing::NotNull());
  device->filterEngine().setMaximumTileExtentForTesting(16);
  RendererGeode renderer(device);
  beginFrame(renderer);
  ASSERT_THAT(device->counters(), testing::NotNull());
  const geode::GeodeCounters* counters = device->counters();

  DrawFilterGraphPastTheSubmissionBound(renderer);
  renderer.endFrame();

  EXPECT_THAT(renderer.deviceLost(), testing::IsFalse());
  EXPECT_THAT(counters->commandBuffers, testing::Gt(16u))
      << "the fixture must actually pass the bound, or it pins nothing";
  EXPECT_THAT(counters->submits, testing::Gt(1u))
      << "a frame past the bound splits across submissions rather than handing the backend a "
         "span it cannot acquire command buffers for";
  EXPECT_THAT(renderer.takeSnapshot().empty(), testing::IsFalse());
}

#ifdef __APPLE__
TEST_F(RendererGeodeTest, NativeMetalBoundsOutstandingFrameSubmissions) {
  std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
  ASSERT_THAT(device, testing::NotNull());
  if (device->runtimeDevice().shaderSourceKind() != gpu::ShaderSourceKind::Msl) {
    GTEST_SKIP() << "requires native Metal";
  }
  RendererGeode renderer(device);
  auto& metal = static_cast<gpu::metal::MetalDevice&>(device->runtimeDevice());
  const auto recordFrame = [&] {
    beginFrame(renderer);
    renderer.setPaint(solidFill(css::RGBA(255, 255, 255, 255)));
    renderer.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});
  };
  recordFrame();
  renderer.endFrame();
  ASSERT_THAT(device->waitForQueueIdle(), testing::Eq(geode::GpuWaitResult::Complete));
  const uint64_t baseline = metal.completedSerial();
  ASSERT_THAT(metal.pauseSubmissionsForTest(), gpu::IsOk());
  recordFrame();
  renderer.endFrame();
  const uint64_t firstFrameBuffers = metal.commandBuffersInFlightForTest();
  for (int frame = 0; frame < 2; ++frame) {
    recordFrame();
    renderer.endFrame();
  }
  EXPECT_THAT(metal.lastSubmittedSerial(), testing::Eq(baseline + 3));
  EXPECT_THAT(metal.completedSerial(), testing::Eq(baseline));

  recordFrame();
  const auto previousWait = metal.lastPendingWaitLookForTest();
  std::jthread releaseOldest([&](std::stop_token stop) {
    while (!stop.stop_requested() && metal.lastPendingWaitLookForTest() == previousWait) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!stop.stop_requested()) {
      metal.releasePausedCommandBuffersForTest(firstFrameBuffers);
    }
  });
  renderer.endFrame();
  releaseOldest.request_stop();
  releaseOldest.join();
  const uint64_t completedAtReturn = metal.completedSerial();
  metal.resumeSubmissionsForTest();

  EXPECT_THAT(renderer.deviceLost(), testing::IsFalse());
  EXPECT_THAT(completedAtReturn, testing::Eq(baseline + 1));
  EXPECT_THAT(metal.lastSubmittedSerial(), testing::Eq(baseline + 4));
}
#endif

TEST_F(RendererGeodeTest, AFrameSplitTakesTheCrossSubmitCompletionWait) {
  std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
  ASSERT_THAT(device, testing::NotNull());
  if (!device->isVulkan()) {
    GTEST_SKIP() << "requires the Vulkan cross-submit completion wait";
  }
  device->filterEngine().setMaximumTileExtentForTesting(16);
  RendererGeode renderer(device);
  beginFrame(renderer);
  ASSERT_THAT(device->counters(), testing::NotNull());
  const geode::GeodeCounters* counters = device->counters();
  // Armed for one wait. A split puts the pass that samples what the submitted buffers wrote in a
  // later submission, which hardware Vulkan does not order automatically, so the split must wait
  // that work out - and a wait that reports a hang must fail the frame closed.
  device->setQueueWaitResultForTesting(geode::GpuWaitResult::TimedOut);

  DrawFilterGraphPastTheSubmissionBound(renderer);
  renderer.endFrame();

  EXPECT_THAT(renderer.deviceLost(), testing::IsTrue());
  EXPECT_THAT(counters->submits, testing::Eq(1u))
      << "the forced split is where the wait runs, so exactly that submission happened and the "
         "frame never reached its own";
  EXPECT_THAT(renderer.takeSnapshot().empty(), testing::IsTrue());
  // A wait on an already-lost device reports the loss without consuming the injection, so
  // disarm it rather than leaving it for whatever waits next.
  device->setQueueWaitResultForTesting(std::nullopt);
}

/// Closing the frame's recorded draws can fail at any encoder retire, which abandons the frame.
/// Every pop that retires an encoder records through the frame right afterwards, so each has to
/// notice the abandonment its own retire caused rather than the one it checked for on entry.
class FrameEncoderCloseFailureTest : public RendererGeodeTest,
                                     public testing::WithParamInterface<const char*> {};

TEST_P(FrameEncoderCloseFailureTest, APopWhoseCloseFailsAbandonsInsteadOfRecording) {
  using namespace components;
  const std::string_view kind = GetParam();
  std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
  ASSERT_THAT(device, testing::NotNull());
  RendererGeode renderer(device);
  beginFrame(renderer);
  ASSERT_THAT(device->counters(), testing::NotNull());
  const geode::GeodeCounters* counters = device->counters();

  if (kind == "IsolatedLayer") {
    renderer.pushIsolatedLayer(0.8, MixBlendMode::Normal);
  } else if (kind == "BlendedLayer") {
    renderer.pushIsolatedLayer(0.8, MixBlendMode::Multiply);
  } else if (kind == "Mask") {
    renderer.pushMask(std::nullopt, MaskType::Luminance);
  } else if (kind == "PatternTile") {
    ASSERT_THAT(renderer.beginPatternTile(Box2d({0, 0}, {32, 32}), Transform2d()),
                testing::IsTrue());
  } else {
    renderer.pushFilterLayer(FilterGraph{}, Box2d({0, 0}, {kViewportSize, kViewportSize}));
  }
  renderer.setPaint(solidFill(css::RGBA(255, 255, 255, 255)));
  renderer.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});
  renderer.injectFrameEncoderCloseFailureForTesting();

  if (kind == "Mask") {
    renderer.transitionMaskToContent();
    renderer.popMask();
  } else if (kind == "PatternTile") {
    renderer.endPatternTile(/*forStroke=*/false, std::nullopt);
  } else if (kind == "Filter") {
    renderer.popFilterLayer();
  } else {
    renderer.popIsolatedLayer();
  }

  EXPECT_THAT(renderer.deviceLost(), testing::IsTrue());
  EXPECT_THAT(renderer.hasActiveDrawingEncoderForTesting(), testing::IsFalse());

  renderer.endFrame();

  EXPECT_THAT(counters->submits, testing::Eq(0u))
      << "a frame abandoned by a failed close must reach the queue with nothing";
  EXPECT_THAT(counters->commandBuffers, testing::Eq(0u));
  EXPECT_THAT(renderer.takeSnapshot().empty(), testing::IsTrue());
}

INSTANTIATE_TEST_SUITE_P(EveryPopThatRetires, FrameEncoderCloseFailureTest,
                         testing::Values("IsolatedLayer", "BlendedLayer", "Mask", "PatternTile",
                                         "Filter"),
                         [](const testing::TestParamInfo<const char*>& info) {
                           return std::string(info.param);
                         });

/// Splitting a frame mid-frame for a memory-limited filter budget has two ways to leave it with
/// no encoder: the retire it starts with, and its own close. The caller reads a refusal as "no
/// room for this filter" and keeps drawing, so both have to abandon rather than report through
/// the return value.
/// @param closesBeforeFailure Closes to let through, choosing which of the two exits fails.
void expectMidFrameSplitFailureAbandonsTheFrame(size_t closesBeforeFailure) {
  std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
  ASSERT_THAT(device, testing::NotNull());
  RendererGeode renderer(device);
  RenderViewport viewport;
  viewport.size = Vector2d(kViewportSize, kViewportSize);
  renderer.beginFrame(viewport);
  ASSERT_THAT(device->counters(), testing::NotNull());
  const geode::GeodeCounters* counters = device->counters();
  renderer.pushIsolatedLayer(0.8, MixBlendMode::Normal);
  renderer.setPaint(solidFill(css::RGBA(255, 255, 255, 255)));
  renderer.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});
  renderer.injectFrameEncoderCloseFailureForTesting(closesBeforeFailure);

  // The return value says nothing here: a forced split reports whether the budget can begin a
  // chunk after it, and this budget never rejected anything. The frame's state is the evidence.
  (void)renderer.submitFilterBudgetChunkForTesting();

  EXPECT_THAT(renderer.deviceLost(), testing::IsTrue());
  // The layer opened before the split still has to unwind, and it records through the frame.
  renderer.popIsolatedLayer();
  EXPECT_THAT(renderer.hasActiveDrawingEncoderForTesting(), testing::IsFalse());
  renderer.endFrame();
  EXPECT_THAT(counters->submits, testing::Eq(0u));
  EXPECT_THAT(counters->commandBuffers, testing::Eq(0u));
  EXPECT_THAT(renderer.takeSnapshot().empty(), testing::IsTrue());
}

TEST_F(RendererGeodeTest, AMidFrameSplitAbandonsWhenItsOpeningRetireFails) {
  expectMidFrameSplitFailureAbandonsTheFrame(/*closesBeforeFailure=*/0);
}

TEST_F(RendererGeodeTest, AMidFrameSplitAbandonsWhenItsOwnCloseFails) {
  expectMidFrameSplitFailureAbandonsTheFrame(/*closesBeforeFailure=*/1);
}

TEST_F(RendererGeodeTest, AnAbandonedFrameSubmitsNothingItRecorded) {
  std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
  ASSERT_THAT(device, testing::NotNull());
  RendererGeode renderer(device);
  beginFrame(renderer);
  ASSERT_THAT(device->counters(), testing::NotNull());
  const geode::GeodeCounters* counters = device->counters();
  const uint64_t serialBefore = device->runtimeDevice().lastSubmittedSerial();
  renderer.setPaint(solidFill(css::RGBA(255, 255, 255, 255)));
  renderer.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});
  components::FilterGraph graph;
  components::FilterNode blur;
  blur.primitive =
      components::filter_primitive::GaussianBlur{.stdDeviationX = 4, .stdDeviationY = 2};
  graph.nodes.push_back(blur);
  renderer.pushFilterLayer(graph, Box2d({0, 0}, {kViewportSize, kViewportSize}));
  renderer.injectFilterFrameSuspensionAndRestoreFailureForTesting();

  renderer.popFilterLayer();
  renderer.endFrame();

  EXPECT_THAT(renderer.deviceLost(), testing::IsTrue());
  EXPECT_THAT(counters->submits, testing::Eq(0u));
  EXPECT_THAT(counters->commandBuffers, testing::Eq(0u));
  EXPECT_THAT(device->runtimeDevice().lastSubmittedSerial(), testing::Eq(serialBefore))
      << "an abandoned frame must consume no submission serial";
  EXPECT_THAT(renderer.takeSnapshot().empty(), testing::IsTrue());
}

TEST_F(RendererGeodeTest, EarlyAbandonmentRetainsAnUnmatchedPathClipMask) {
  std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
  ASSERT_THAT(device, testing::NotNull());
  RendererGeode renderer(device);
  beginFrame(renderer);
  ASSERT_THAT(device->counters(), testing::NotNull());

  ResolvedClip clip;
  ClipPathShape shape;
  shape.path = PathBuilder().addRect(Box2d::FromXYWH(0.0, 0.0, 32.0, 32.0)).build();
  clip.clipPaths.push_back(std::move(shape));
  const uint64_t textureCreatesBefore = device->counters()->textureCreates;
  renderer.pushClip(clip);
  ASSERT_GT(device->counters()->textureCreates, textureCreatesBefore);

  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  renderer.drawRect(Box2d::FromXYWH(0.0, 0.0, kViewportSize, kViewportSize), StrokeParams{});
  renderer.injectFrameEncoderCloseFailureForTesting();
  (void)renderer.submitFilterBudgetChunkForTesting();
  ASSERT_THAT(renderer.deviceLost(), testing::IsTrue());

  const std::size_t pooledBefore = renderer.texturePoolStats().textureCount;
  const std::size_t retainedBefore = renderer.failedFilterTextureCountForTesting();
  renderer.endFrame();

  EXPECT_EQ(renderer.texturePoolStats().textureCount, pooledBefore);
  EXPECT_GT(renderer.failedFilterTextureCountForTesting(), retainedBefore);
}

TEST_F(RendererGeodeTest, RefusalAfterAdmissionPreservesTheParentPixels) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);
  renderer.setPaint(solidFill(css::RGBA(255, 0, 255, 255)));
  const Box2d region({0, 0}, {kViewportSize, kViewportSize});
  renderer.drawRect(region, StrokeParams{});
  components::FilterGraph graph;
  components::FilterNode flood;
  flood.primitive =
      components::filter_primitive::Flood{.floodColor = css::Color(css::RGBA(0, 0, 255, 255))};
  graph.nodes.push_back(flood);
  renderer.pushFilterLayer(graph, region);
  ASSERT_EQ(renderer.resourceStats().filterExecutions, 1u);
  // Lowering the limit after admission forces the actual allocation-refusal path.
  renderer.setSurfaceBudgetForTesting(2, RendererSurfaceBudget::kMaximumBytes);
  renderer.popFilterLayer();
  renderer.endFrame();
  EXPECT_TRUE(renderer.resourceStats().surfaceBudgetRejected);
  const size_t width = static_cast<size_t>(kViewportSize);
  RendererBitmap expected{Vector2i(kViewportSize, kViewportSize),
                          std::vector<uint8_t>(width * width * 4), width * 4};
  for (size_t offset = 0; offset < expected.pixels.size(); offset += 4) {
    expected.pixels[offset] = expected.pixels[offset + 2] = expected.pixels[offset + 3] = 255;
  }
  editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), expected,
                                       "late_filter_refusal_parent",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST_F(RendererGeodeTest, ObjectBoundingBoxHaloUsesTheExecutedScalingOrder) {
  using namespace components;
  for (bool nearAxisShear : {false, true}) {
    SCOPED_TRACE(nearAxisShear);
    FilterGraph graph;
    graph.primitiveUnits = PrimitiveUnits::ObjectBoundingBox;
    graph.elementBoundingBox = Box2d({0, 0}, nearAxisShear ? Vector2d(1e7, 1) : Vector2d(1.1, 1.1));
    FilterNode fill;
    fill.primitive =
        filter_primitive::Flood{.floodColor = css::Color(css::RGBA(255, 255, 255, 255))};
    graph.nodes.push_back(fill);
    FilterNode blur;
    blur.primitive =
        filter_primitive::GaussianBlur{.stdDeviationX = nearAxisShear ? 0.5 : 1.9782261838087567,
                                       .stdDeviationY = nearAxisShear ? 0.0 : 1.9782261838087567};
    graph.nodes.push_back(blur);
    std::shared_ptr<geode::GeodeDevice> referenceDevice = geode::GeodeDevice::CreateHeadless();
    std::shared_ptr<geode::GeodeDevice> tiledDevice = geode::GeodeDevice::CreateHeadless();
    ASSERT_TRUE(referenceDevice);
    ASSERT_TRUE(tiledDevice);
    tiledDevice->filterEngine().setMaximumTileExtentForTesting(16);
    const auto render = [&](const std::shared_ptr<geode::GeodeDevice>& device) {
      RendererGeode renderer(device);
      beginFrame(renderer);
      Transform2d transform = Transform2d::Scale(1.1);
      if (nearAxisShear) {
        transform.data[0] = 1e-9;
        transform.data[1] = 5e-7;
        transform.data[3] = 1;
      }
      if (device == tiledDevice) {
        const auto plan =
            device->filterEngine().executionPlan(graph, kViewportSize, kViewportSize, transform);
        // Three radius-two box passes require six source pixels on either side.
        EXPECT_GE((plan.tileWidth - plan.coreWidth) / 2, 6u);
      }
      renderer.setTransform(transform);
      renderer.pushFilterLayer(
          graph,
          Box2d({0, 0}, {nearAxisShear ? kViewportSize / 1e-9 : kViewportSize, kViewportSize}));
      renderer.setTransform(Transform2d());
      renderer.setPaint(solidFill(css::RGBA(255, 255, 255, 255)));
      renderer.drawRect(Box2d({-4, -4}, {kViewportSize + 4, kViewportSize + 4}), StrokeParams{});
      renderer.popFilterLayer();
      renderer.endFrame();
      return renderer.takeSnapshot();
    };
    const auto expected = render(referenceDevice);
    const auto actual = render(tiledDevice);
    ASSERT_THAT(PixelAt(expected, 32, 32), Rgba(255, 255, 255, 255));
    ASSERT_EQ(referenceDevice->filterEngine().lastExecutionMemory().tileExecutions, 1u);
    ASSERT_GT(tiledDevice->filterEngine().lastExecutionMemory().tileExecutions, 1u);
    editor::tests::CompareBitmapToBitmap(actual, expected, "object_bounds_halo_rounding",
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST_F(RendererGeodeTest, LostHeadlessDeviceIsNotReused) {
  const auto render = [this](RendererGeode& renderer) {
    beginFrame(renderer);
    renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
    renderer.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});
    renderer.endFrame();
    return renderer.takeSnapshot();
  };
  RendererGeode reference = createRenderer();
  const auto expected = render(reference);
  ASSERT_THAT(expected.dimensions, testing::Eq(Vector2i(64, 64)));
  {
    RendererGeode failed;
    editor::tests::CompareBitmapToBitmap(render(failed), expected, "pool_before_device_loss",
                                         editor::tests::PixelmatchIdentityParams());
    failed.injectDeviceLossForTesting();
    ASSERT_THAT(failed.deviceLost(), testing::IsTrue());
  }
  RendererGeode recovered;
  EXPECT_THAT(recovered.deviceLost(), testing::IsFalse());
  editor::tests::CompareBitmapToBitmap(render(recovered), expected, "pool_after_device_loss",
                                       editor::tests::PixelmatchIdentityParams());
}

/// Draws one opaque frame through \p renderer, which submits it to the GPU.
/// @param renderer Renderer to draw with.
void DrawOpaqueFrame(RendererGeode& renderer) {
  RenderViewport viewport;
  viewport.size = Vector2d(kViewportSize, kViewportSize);
  viewport.devicePixelRatio = 1.0;
  renderer.beginFrame(viewport);
  renderer.setPaint(solidFill(css::RGBA(0, 128, 255, 255)));
  renderer.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});
  renderer.endFrame();
}

TEST_F(RendererGeodeTest, WaitForSubmittedWorkCompletesAfterADrawnFrameWithoutReadback) {
  std::shared_ptr<geode::GeodeDevice> device = sharedDevice();
  ASSERT_THAT(device, testing::NotNull());
  RendererGeode renderer(device);
  (void)renderer.consumeReadbackStats();

  DrawOpaqueFrame(renderer);
  const uint64_t frameSerial = device->runtimeDevice().lastSubmittedSerial();
  ASSERT_THAT(frameSerial, testing::Gt(0u)) << "the frame must reach the GPU, or this pins nothing";

  EXPECT_THAT(renderer.waitForSubmittedWork(), testing::Eq(geode::GpuWaitResult::Complete));
  EXPECT_THAT(device->runtimeDevice().completedSerial(), testing::Ge(frameSerial))
      << "a completed wait must leave the frame's submission finished on the GPU";
  EXPECT_THAT(renderer.deviceLost(), testing::IsFalse());
  const RendererReadbackStats stats = renderer.consumeReadbackStats();
  EXPECT_THAT(stats.count, testing::Eq(0)) << "waiting for GPU completion must not read pixels";
  EXPECT_THAT(stats.bufferCreates, testing::Eq(0u));
}

TEST_F(RendererGeodeTest, WaitForSubmittedWorkWithNothingOutstandingCompletesAtOnce) {
  RendererGeode withoutDevice(std::shared_ptr<geode::GeodeDevice>{});
  EXPECT_THAT(withoutDevice.waitForSubmittedWork(std::chrono::milliseconds::zero()),
              testing::Eq(geode::GpuWaitResult::Complete))
      << "a renderer without a device has submitted nothing";

  RendererGeode renderer = createRenderer();
  ASSERT_THAT(renderer.waitForSubmittedWork(), testing::Eq(geode::GpuWaitResult::Complete));
  // With everything already finished there is nothing left to make progress, so even a zero stall
  // bound must report completion rather than a stall.
  EXPECT_THAT(renderer.waitForSubmittedWork(std::chrono::milliseconds::zero()),
              testing::Eq(geode::GpuWaitResult::Complete));
  EXPECT_THAT(renderer.deviceLost(), testing::IsFalse());
}

TEST_F(RendererGeodeTest, WaitForSubmittedWorkReportsALostDeviceWithoutWaiting) {
  RendererGeode renderer;
  DrawOpaqueFrame(renderer);
  renderer.injectDeviceLossForTesting();
  ASSERT_THAT(renderer.deviceLost(), testing::IsTrue());

  EXPECT_THAT(renderer.waitForSubmittedWork(), testing::Eq(geode::GpuWaitResult::DeviceLost));
}

TEST_F(RendererGeodeTest, WaitForSubmittedWorkThatStallsDeclaresTheDeviceLost) {
  std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
  ASSERT_THAT(device, testing::NotNull());
  RendererGeode renderer(device);
  DrawOpaqueFrame(renderer);
  (void)renderer.consumeReadbackStats();
  // Stands in for outstanding work that stops progressing; the device's queue-idle wait applies
  // the same loss declaration it applies to a real stall.
  device->setQueueWaitResultForTesting(geode::GpuWaitResult::TimedOut);

  EXPECT_THAT(renderer.waitForSubmittedWork(), testing::Eq(geode::GpuWaitResult::TimedOut));
  EXPECT_THAT(renderer.deviceLost(), testing::IsTrue());
  const RendererReadbackStats stats = renderer.consumeReadbackStats();
  EXPECT_THAT(stats.timedOutWaitSite, testing::Eq(GpuWaitTimeoutSite::QueueIdle));
  EXPECT_THAT(renderer.waitForSubmittedWork(), testing::Eq(geode::GpuWaitResult::DeviceLost))
      << "a later wait on the lost device must report the loss without waiting again";
}

TEST_F(RendererGeodeTest, NestedParameterGrowthPreservesOuterStripAdmission) {
  using namespace components;
  constexpr uint32_t kWidth = 1024;
  constexpr uint32_t kHeight = 64;
  std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
  ASSERT_TRUE(device);
  RendererGeode renderer(device);
  RenderViewport viewport;
  viewport.size = Vector2d(kWidth, kHeight);
  renderer.beginFrame(viewport);
  FilterGraph outer;
  FilterNode blur;
  blur.primitive = filter_primitive::GaussianBlur{
      .stdDeviationX = 96,
      .stdDeviationY = 96,
      .edgeMode = filter_primitive::GaussianBlur::EdgeMode::Duplicate};
  outer.nodes.push_back(blur);
  FilterGraph inner;
  filter_primitive::ComponentTransfer transfer;
  transfer.funcR.type = transfer.funcG.type = transfer.funcB.type =
      filter_primitive::ComponentTransfer::FuncType::Table;
  transfer.funcR.tableValues.resize(kMaximumFilterTableValues, 1.0);
  transfer.funcG.tableValues.resize(kMaximumFilterTableValues, 0.0);
  transfer.funcB.tableValues.resize(kMaximumFilterTableValues, 0.0);
  FilterNode color;
  color.primitive = transfer;
  inner.nodes.resize(kMaximumFilterGraphNodes, color);
  const Box2d region({0, 0}, {kWidth, kHeight});
  const auto admitted = device->filterEngine().executionPlan(outer, kWidth, kHeight, Transform2d());
  ASSERT_GT(admitted.tiles, 1u);
  ASSERT_EQ(admitted.tileHeight, kHeight);
  ASSERT_EQ(admitted.coreHeight, kHeight);
  ASSERT_LT(admitted.coreWidth, kWidth);
  ASSERT_GT(admitted.tileWidth, admitted.coreWidth);
  renderer.pushFilterLayer(outer, region);
  const uint64_t before = device->filterEngine().retainedBufferBytes();
  renderer.pushFilterLayer(inner, region);
  renderer.setPaint(solidFill(css::RGBA(255, 255, 255, 255)));
  renderer.drawRect(region, StrokeParams{});
  renderer.popFilterLayer();
  ASSERT_GT(device->filterEngine().retainedBufferBytes(), before);
  const auto replanned =
      device->filterEngine().executionPlan(outer, kWidth, kHeight, Transform2d());
  EXPECT_EQ(replanned.tileWidth, admitted.tileWidth);
  EXPECT_EQ(replanned.tileHeight, admitted.tileHeight);
  EXPECT_EQ(replanned.tiles, admitted.tiles);
  renderer.popFilterLayer();
  EXPECT_EQ(device->filterEngine().lastExecutionMemory().tileExecutions, admitted.tiles);
  renderer.endFrame();
  EXPECT_FALSE(renderer.resourceStats().filterBudgetRejected);
  EXPECT_FALSE(renderer.resourceStats().surfaceBudgetRejected);
  RendererBitmap expected{Vector2i(kWidth, kHeight), std::vector<uint8_t>(kWidth * kHeight * 4),
                          kWidth * 4};
  for (size_t offset = 0; offset < expected.pixels.size(); offset += 4) {
    expected.pixels[offset] = expected.pixels[offset + 3] = 255;
  }
  editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), expected, "nested_parameter_growth",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST_F(RendererGeodeTest, LargeBlurStripTilesMatchUntiledPixels) {
  const std::string source = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="768" height="64">
    <defs><filter id="f" filterUnits="userSpaceOnUse" x="0" y="0" width="768" height="64">
      <feGaussianBlur stdDeviation="96"/>
    </filter></defs><g filter="url(#f)">
      <rect width="317" height="64" fill="#3973ad" opacity="0.7"/>
      <rect x="280" y="13.2" width="450" height="26.5" fill="#b75d23" opacity="0.4"/>
    </g></svg>)svg";
  std::shared_ptr<geode::GeodeDevice> referenceDevice = geode::GeodeDevice::CreateHeadless();
  std::shared_ptr<geode::GeodeDevice> tiledDevice = geode::GeodeDevice::CreateHeadless();
  ASSERT_TRUE(referenceDevice);
  ASSERT_TRUE(tiledDevice);
  referenceDevice->filterEngine().setMaximumTileExtentForTesting(16);
  ParseWarningSink warnings;
  auto referenceDocument = parser::SVGParser::ParseSVG(source, warnings);
  auto tiledDocument = parser::SVGParser::ParseSVG(source, warnings);
  ASSERT_TRUE(referenceDocument.hasResult());
  ASSERT_TRUE(tiledDocument.hasResult());
  ASSERT_THAT(warnings.warnings(), testing::IsEmpty());
  RendererGeode reference(referenceDevice);
  RendererGeode tiled(tiledDevice);
  reference.draw(referenceDocument.result());
  tiled.draw(tiledDocument.result());
  components::FilterGraph graph;
  components::FilterNode blur;
  blur.primitive =
      components::filter_primitive::GaussianBlur{.stdDeviationX = 96, .stdDeviationY = 96};
  graph.nodes.push_back(blur);
  const auto plan = tiledDevice->filterEngine().executionPlan(graph, 768, 64, Transform2d());
  ASSERT_EQ(plan.tileHeight, 64u);
  ASSERT_EQ(plan.coreHeight, 64u);
  ASSERT_LT(plan.coreWidth, 768u);
  ASSERT_GT(plan.tileWidth, plan.coreWidth);
  ASSERT_GT(plan.tiles, 1u);
  EXPECT_FALSE(reference.resourceStats().filterBudgetRejected);
  EXPECT_FALSE(tiled.resourceStats().filterBudgetRejected);
  ASSERT_EQ(referenceDevice->filterEngine().lastExecutionMemory().tileExecutions, 1u);
  ASSERT_GT(tiledDevice->filterEngine().lastExecutionMemory().tileExecutions, 1u);
  editor::tests::CompareBitmapToBitmap(tiled.takeSnapshot(), reference.takeSnapshot(),
                                       "large_blur_strip_tiles",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST_F(RendererGeodeTest, FilterTilesPreservePixelsAcrossSamplingAndClipBoundaries) {
  const std::array<const char*, 7> graphs = {
      R"(<feGaussianBlur stdDeviation="0.7 1.2"/>)",
      R"(<feGaussianBlur stdDeviation="3.4 2.3"/>)",
      R"(<feMorphology operator="dilate" radius="4 3"/><feOffset dx="-1.3" dy="2.7"/>)",
      R"(<feGaussianBlur stdDeviation="1.1" result="blurred"/>
          <feComposite in="SourceGraphic" in2="blurred" operator="arithmetic" k2="0.4" k3="0.6"/>)",
      R"(<feColorMatrix type="saturate" values="0.7" result="color"/>
          <feOffset dx="3.3" dy="-2.2" x="5.2" y="4.1" width="52.4" height="45.3"/>
          <feMerge color-interpolation-filters="sRGB"><feMergeNode/><feMergeNode in="color"/></feMerge>)",
      R"(<feDropShadow dx="2" dy="-1" stdDeviation="1.2" flood-opacity="0.7"/>)",
      R"(<feConvolveMatrix order="3" kernelMatrix="0 1 0 1 4 1 0 1 0" divisor="8" edgeMode="duplicate"/>)",
  };
  std::shared_ptr<geode::GeodeDevice> referenceDevice = geode::GeodeDevice::CreateHeadless();
  std::shared_ptr<geode::GeodeDevice> tiledDevice = geode::GeodeDevice::CreateHeadless();
  ASSERT_TRUE(referenceDevice);
  ASSERT_TRUE(tiledDevice);
  tiledDevice->filterEngine().setMaximumTileExtentForTesting(64);
  for (size_t index = 0; index < graphs.size(); ++index) {
    SCOPED_TRACE(index);
    const std::string source =
        std::string(R"svg(<svg xmlns="http://www.w3.org/2000/svg"
        width="141" height="117" viewBox="0 0 100 70" preserveAspectRatio="none"><defs><filter id="f"
        filterUnits="userSpaceOnUse" x="0" y="0" width="100" height="83">)svg") +
        graphs[index] +
        R"svg(</filter></defs><g filter="url(#f)"><rect x="0" y="0" width="30.3" height="83"
        fill="#3973ad" opacity="0.7"/><rect x="28" y="14" width="65" height="31"
        fill="#b75d23" opacity="0.4"/><circle cx="70" cy="68" r="14" fill="#31ba55"/></g></svg>)svg";
    ParseWarningSink warnings;
    auto referenceDocument = parser::SVGParser::ParseSVG(source, warnings);
    auto tiledDocument = parser::SVGParser::ParseSVG(source, warnings);
    ASSERT_TRUE(referenceDocument.hasResult());
    ASSERT_TRUE(tiledDocument.hasResult());
    ASSERT_THAT(warnings.warnings(), testing::IsEmpty());
    RendererGeode reference(referenceDevice);
    RendererGeode tiled(tiledDevice);
    reference.draw(referenceDocument.result());
    tiled.draw(tiledDocument.result());
    ASSERT_GT(tiledDevice->filterEngine().lastExecutionMemory().tileExecutions, 1u);
    ASSERT_EQ(referenceDevice->filterEngine().lastExecutionMemory().tileExecutions, 1u);
    editor::tests::CompareBitmapToBitmap(tiled.takeSnapshot(), reference.takeSnapshot(),
                                         "filter_tile_boundaries_" + std::to_string(index),
                                         editor::tests::PixelmatchIdentityParams());
  }
}

/// A document whose one filter, made of \p primitives, covers three overlapping shapes.
/// @param primitives Filter primitive markup.
std::string FilteredShapesSvg(std::string_view primitives) {
  return std::string(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="96" height="80"
      viewBox="0 0 96 80"><defs><filter id="f" filterUnits="userSpaceOnUse" x="0" y="0"
      width="96" height="80">)svg") +
         std::string(primitives) +
         R"svg(</filter></defs><g filter="url(#f)"><rect x="8" y="10" width="40" height="44"
      fill="#3973ad"/><circle cx="58" cy="44" r="22" fill="#b75d23" opacity="0.6"/><rect x="30"
      y="50" width="50" height="18" fill="#31ba55"/></g></svg>)svg";
}

/// Parses \p source, failing the test when it does not parse cleanly.
/// @param source SVG document source.
std::optional<SVGDocument> ParseFilterDocument(const std::string& source) {
  ParseWarningSink warnings;
  auto parsed = parser::SVGParser::ParseSVG(source, warnings);
  EXPECT_THAT(parsed, NoParseError());
  EXPECT_THAT(warnings.warnings(), testing::IsEmpty());
  if (!parsed.hasResult()) {
    return std::nullopt;
  }
  return std::move(parsed.result());
}

/// \p source rendered once on a device of its own, whose filter engine starts with nothing cached.
/// @param source SVG document source.
/// @param maximumTileExtent When nonzero, the largest tile the filter engine executes, which must
///   split the filter into several tiles; zero keeps the engine's own extent.
RendererBitmap RenderOnFreshDevice(const std::string& source, uint32_t maximumTileExtent = 0) {
  const std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
  EXPECT_THAT(device, testing::NotNull());
  std::optional<SVGDocument> document = ParseFilterDocument(source);
  if (!device || !document) {
    return {};
  }
  if (maximumTileExtent != 0) {
    device->filterEngine().setMaximumTileExtentForTesting(maximumTileExtent);
  }
  RendererGeode renderer(device);
  renderer.draw(*document);
  if (maximumTileExtent != 0) {
    EXPECT_THAT(device->filterEngine().lastExecutionMemory().tileExecutions, testing::Gt(1u))
        << "Premise: the filter must execute in tiles";
  }
  return renderer.takeSnapshot();
}

/// Primitives that flood sixteen opaque squares of distinct colors that do not overlap, each with
/// the result `square<i>`.
std::string SixteenFloodedSquares() {
  std::string primitives;
  for (int i = 0; i < 16; ++i) {
    primitives += "<feFlood x=\"" + std::to_string(4 + 22 * (i % 4)) + "\" y=\"" +
                  std::to_string(4 + 18 * (i / 4)) +
                  "\" width=\"14\" height=\"12\" flood-color=\"rgb(" + std::to_string(30 + 14 * i) +
                  "," + std::to_string(220 - 12 * i) + "," + std::to_string(60 + (i % 3) * 70) +
                  ")\" result=\"square" + std::to_string(i) + "\"/>";
  }
  return primitives;
}

/// An feMerge whose children read the results named by \p children, in order.
std::string MergeOf(const std::vector<std::string>& children) {
  std::string merge = "<feMerge>";
  for (const std::string& child : children) {
    merge += "<feMergeNode in=\"" + child + "\"/>";
  }
  return merge + "</feMerge>";
}

/// Four feMerge primitives that each read one square sixteen times, then one that reads all
/// sixteen squares: 80 inputs, more than the execution keeps inline. With \p repeated false, the
/// first four read their square once, which draws the same picture from 20 inputs, and
/// \p misreadChild replaces the final merge's last child with that result.
std::string ManyInputMerges(bool repeated, const std::string& misreadChild = "") {
  std::string primitives = SixteenFloodedSquares();
  for (int merge = 0; merge < 4; ++merge) {
    primitives +=
        MergeOf(std::vector<std::string>(repeated ? 16 : 1, "square" + std::to_string(merge)));
  }
  std::vector<std::string> squares;
  for (int i = 0; i < 16; ++i) {
    squares.push_back("square" + std::to_string(i));
  }
  if (!misreadChild.empty()) {
    squares.back() = misreadChild;
  }
  return primitives + MergeOf(squares);
}

// A named input reads the latest earlier primitive with that result name, and a name no earlier
// primitive has reads the previous result, the source graphic for the first primitive. A keyword
// input reads the standard input even when a primitive's result has the keyword's name. The engine
// resolves each name to the position of the primitive that produces it once per execution and
// keeps results by that position, so each graph here must render exactly like the same graph with
// every result named once, whole and split into tiles, which start their results afresh. The
// input-derived default subregion follows the same resolution, and a graph with more inputs than
// the execution keeps inline resolves the same way. Each case also differs from what a wrong
// resolution would draw, or it could not tell the two apart.
TEST_F(RendererGeodeTest, NamedInputsReadTheLatestEarlierPrimitiveWithThatName) {
  struct Case {
    std::string_view name;
    std::string_view sharedNames;  //!< Graph whose result names repeat or are missing.
    std::string_view uniqueNames;  //!< The same graph with every result named once.
    std::string_view misread;      //!< What a wrong resolution of the names would draw.
  };
  const std::string manyInputs = ManyInputMerges(/*repeated=*/true);
  const std::string fewInputs = ManyInputMerges(/*repeated=*/false);
  const std::string manyInputsMisread = ManyInputMerges(/*repeated=*/false, "square0");
  const std::array<Case, 8> cases = {{
      {"redefined_name",
       R"(<feOffset in="SourceGraphic" dx="6" dy="2" result="a"/>
          <feGaussianBlur in="SourceGraphic" stdDeviation="1.5" result="a"/>
          <feColorMatrix in="SourceGraphic" type="saturate" values="0.2" result="b"/>
          <feMerge><feMergeNode in="b"/><feMergeNode in="a"/></feMerge>)",
       R"(<feOffset in="SourceGraphic" dx="6" dy="2" result="a0"/>
          <feGaussianBlur in="SourceGraphic" stdDeviation="1.5" result="a1"/>
          <feColorMatrix in="SourceGraphic" type="saturate" values="0.2" result="b"/>
          <feMerge><feMergeNode in="b"/><feMergeNode in="a1"/></feMerge>)",
       R"(<feOffset in="SourceGraphic" dx="6" dy="2" result="a0"/>
          <feGaussianBlur in="SourceGraphic" stdDeviation="1.5" result="a1"/>
          <feColorMatrix in="SourceGraphic" type="saturate" values="0.2" result="b"/>
          <feMerge><feMergeNode in="b"/><feMergeNode in="a0"/></feMerge>)"},
      {"redefined_name_sets_the_default_subregion",
       R"(<feFlood x="8" y="8" width="24" height="20" flood-color="#c03030" result="r"/>
          <feFlood x="40" y="30" width="40" height="36" flood-color="#3060c0" result="r"/>
          <feOffset in="r" dx="3" dy="2"/>)",
       R"(<feFlood x="8" y="8" width="24" height="20" flood-color="#c03030" result="r0"/>
          <feFlood x="40" y="30" width="40" height="36" flood-color="#3060c0" result="r1"/>
          <feOffset in="r1" dx="3" dy="2"/>)",
       R"(<feFlood x="8" y="8" width="24" height="20" flood-color="#c03030" result="r0"/>
          <feFlood x="40" y="30" width="40" height="36" flood-color="#3060c0" result="r1"/>
          <feOffset in="r1" x="8" y="8" width="24" height="20" dx="3" dy="2"/>)"},
      {"input_named_like_its_own_result",
       R"(<feOffset dx="5" dy="3" result="s"/>
          <feGaussianBlur in="s" stdDeviation="1.5" result="s"/>
          <feComposite in="s" in2="SourceGraphic" operator="xor"/>)",
       R"(<feOffset dx="5" dy="3" result="s0"/>
          <feGaussianBlur in="s0" stdDeviation="1.5" result="s1"/>
          <feComposite in="s1" in2="SourceGraphic" operator="xor"/>)",
       R"(<feOffset dx="5" dy="3" result="s0"/>
          <feGaussianBlur in="s0" stdDeviation="1.5" result="s1"/>
          <feComposite in="s0" in2="SourceGraphic" operator="xor"/>)"},
      {"name_defined_only_later",
       R"(<feOffset dx="4" dy="-3" result="x"/>
          <feGaussianBlur in="later" stdDeviation="1.5"/>
          <feComposite in2="x" operator="over" result="later"/>)",
       R"(<feOffset dx="4" dy="-3" result="x"/>
          <feGaussianBlur stdDeviation="1.5"/>
          <feComposite in2="x" operator="over" result="later"/>)",
       R"(<feOffset dx="4" dy="-3" result="x"/>
          <feGaussianBlur in="SourceGraphic" stdDeviation="1.5"/>
          <feComposite in2="x" operator="over" result="later"/>)"},
      {"name_no_primitive_defines_read_first",
       R"(<feGaussianBlur in="nowhere" stdDeviation="1.5"/>
          <feOffset dx="3" dy="2"/>)",
       R"(<feGaussianBlur stdDeviation="1.5"/>
          <feOffset dx="3" dy="2"/>)",
       R"(<feGaussianBlur in="SourceAlpha" stdDeviation="1.5"/>
          <feOffset dx="3" dy="2"/>)"},
      {"result_named_like_a_keyword",
       R"(<feFlood flood-color="#c03030" result="SourceAlpha"/>
          <feComposite in="SourceAlpha" in2="SourceGraphic" operator="xor"/>)",
       R"(<feFlood flood-color="#c03030" result="flooded"/>
          <feComposite in="SourceAlpha" in2="SourceGraphic" operator="xor"/>)",
       R"(<feFlood flood-color="#c03030" result="flooded"/>
          <feComposite in="flooded" in2="SourceGraphic" operator="xor"/>)"},
      {"more_inputs_than_the_inline_tables", manyInputs, fewInputs, manyInputsMisread},
      {"result_read_after_later_primitives",
       R"(<feOffset in="SourceGraphic" dx="-5" dy="4" result="kept"/>
          <feGaussianBlur in="SourceGraphic" stdDeviation="1"/>
          <feColorMatrix type="hueRotate" values="120"/>
          <feMorphology operator="dilate" radius="1"/>
          <feOffset dx="3" dy="1" result="chain"/>
          <feComposite in="kept" in2="chain" operator="arithmetic" k2="0.5" k3="0.7"/>)",
       R"(<feGaussianBlur in="SourceGraphic" stdDeviation="1"/>
          <feColorMatrix type="hueRotate" values="120"/>
          <feMorphology operator="dilate" radius="1"/>
          <feOffset dx="3" dy="1" result="chain"/>
          <feOffset in="SourceGraphic" dx="-5" dy="4" result="kept"/>
          <feComposite in="kept" in2="chain" operator="arithmetic" k2="0.5" k3="0.7"/>)",
       R"(<feGaussianBlur in="SourceGraphic" stdDeviation="1"/>
          <feColorMatrix type="hueRotate" values="120"/>
          <feMorphology operator="dilate" radius="1"/>
          <feOffset dx="3" dy="1" result="chain"/>
          <feComposite in="chain" in2="chain" operator="arithmetic" k2="0.5" k3="0.7"/>)"},
  }};
  constexpr uint32_t kTileExtent = 32;
  for (const Case& testCase : cases) {
    SCOPED_TRACE(testCase.name);
    const std::string label(testCase.name);
    const RendererBitmap expected = RenderOnFreshDevice(FilteredShapesSvg(testCase.uniqueNames));
    editor::tests::BitmapGoldenCompareParams signalParams =
        editor::tests::PixelmatchIdentityParams();
    signalParams.maxMismatchedPixels = std::numeric_limits<int>::max();
    RendererBitmap transparent = expected;
    std::fill(transparent.pixels.begin(), transparent.pixels.end(), uint8_t{0});
    int drawnPixels = -1;
    editor::tests::CompareBitmapToBitmap(expected, transparent, "named_inputs_drawn_" + label,
                                         signalParams, &drawnPixels);
    ASSERT_THAT(drawnPixels, testing::Gt(0))
        << "Premise: the graph with every result named once must draw";
    int misreadPixels = -1;
    editor::tests::CompareBitmapToBitmap(RenderOnFreshDevice(FilteredShapesSvg(testCase.misread)),
                                         expected, "named_inputs_premise_" + label, signalParams,
                                         &misreadPixels);
    ASSERT_THAT(misreadPixels, testing::Gt(0))
        << "Premise: a wrong resolution of the names must change the rendering";

    editor::tests::CompareBitmapToBitmap(
        RenderOnFreshDevice(FilteredShapesSvg(testCase.sharedNames)), expected,
        "named_inputs_" + label, editor::tests::PixelmatchIdentityParams());
    editor::tests::CompareBitmapToBitmap(
        RenderOnFreshDevice(FilteredShapesSvg(testCase.sharedNames), kTileExtent),
        RenderOnFreshDevice(FilteredShapesSvg(testCase.uniqueNames), kTileExtent),
        "named_inputs_tiled_" + label, editor::tests::PixelmatchIdentityParams());
  }
}

/// A document whose markers sit inside \p depth nested viewports turned by one rotation, one
/// marker on each of the \p placements vertices of a polyline. Every viewport and every marker
/// clips to a turned rectangle, so each pushes a clip polygon. The viewports shrink by one unit on
/// each side, so each polygon lies inside the one before it and no intersection is empty.
/// @param depth Number of nested viewports.
/// @param placements Number of polyline vertices, one marker each.
std::string MarkersInTurnedViewportsSvg(int depth, int placements) {
  std::string source = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
      <defs><marker id="m" markerUnits="userSpaceOnUse" markerWidth="4" markerHeight="4"
      refX="2" refY="2"><rect width="4" height="4" fill="blue"/></marker></defs>
      <g transform="rotate(30 50 50)"><svg x="5" y="5" width="90" height="90">)svg";
  for (int level = 1; level < depth; ++level) {
    const int size = 90 - 2 * level;
    source += "<svg x=\"1\" y=\"1\" width=\"" + std::to_string(size) + "\" height=\"" +
              std::to_string(size) + "\">";
  }
  const int innermost = 90 - 2 * (depth - 1);
  source += R"svg(<polyline fill="none" marker-start="url(#m)" marker-mid="url(#m)" )svg"
            R"svg(marker-end="url(#m)" points=")svg";
  for (int i = 0; i < placements; ++i) {
    const double x = 4.0 + (i % 2) * (innermost - 8);
    const double y = 4.0 + (innermost - 8) * static_cast<double>(i) / (placements - 1);
    source += (i == 0 ? "" : " ") + std::to_string(x) + "," + std::to_string(y);
  }
  source += "\"/>";
  for (int level = 0; level < depth; ++level) {
    source += "</svg>";
  }
  source += "</g></svg>";
  return source;
}

// Each pushed clip folds its polygon into the fold cached on the entry below it, so the clip stack
// performs one polygon intersection per pushed polygon that has a polygon beneath it: the
// viewports after the first, and every marker. Popping a marker's clip, and reapplying the clip
// state for each marker's draw, must not fold the stack again. The count is exact and does not
// grow with the product of depth and placements.
TEST_F(RendererGeodeTest, ClipStackFoldsOncePerPushedPolygon) {
  for (const auto [depth, placements] : {std::pair{20, 200}, std::pair{40, 200}}) {
    SCOPED_TRACE(testing::Message() << "depth " << depth << ", placements " << placements);
    std::optional<SVGDocument> document =
        ParseFilterDocument(MarkersInTurnedViewportsSvg(depth, placements));
    ASSERT_TRUE(document.has_value());
    RendererGeode renderer = createRenderer();
    renderer.draw(*document);
    const auto expected = static_cast<uint64_t>((depth - 1) + placements);
    EXPECT_EQ(renderer.lastFrameTimings().counters.clipPolygonIntersections, expected);
  }
}

// Filter passes reuse their parameter slots across frames and skip uploading bytes a slot already
// holds. A document whose filter has the same passes with other parameters lands in the same slots
// and binds the same textures, so its passes reuse every bind group; each changed parameter still
// has to reach the GPU. Both blur deviations stay below the box-blur threshold of 2, so the edit
// keeps the pass structure.
TEST_F(RendererGeodeTest, ReusedFilterSlotsUploadEditedParameters) {
  const std::string original = FilteredShapesSvg(
      R"(<feGaussianBlur stdDeviation="1.5" result="soft"/>
         <feOffset dx="3" dy="-2" result="moved"/>
         <feDropShadow in="soft" dx="2" dy="3" stdDeviation="1" flood-color="#802040"/>
         <feComposite in2="moved" operator="arithmetic" k1="0.2" k2="0.5" k3="0.5"/>)");
  const std::string edited = FilteredShapesSvg(
      R"(<feGaussianBlur stdDeviation="1.8" result="soft"/>
         <feOffset dx="-4" dy="1" result="moved"/>
         <feDropShadow in="soft" dx="-1" dy="2" stdDeviation="1.6" flood-color="#208040"/>
         <feComposite in2="moved" operator="arithmetic" k1="0.1" k2="0.7" k3="0.4"/>)");
  std::optional<SVGDocument> originalDocument = ParseFilterDocument(original);
  std::optional<SVGDocument> editedDocument = ParseFilterDocument(edited);
  ASSERT_THAT(originalDocument, testing::Optional(testing::_));
  ASSERT_THAT(editedDocument, testing::Optional(testing::_));

  const std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
  ASSERT_THAT(device, testing::NotNull());
  RendererGeode renderer(device);
  renderer.draw(*originalDocument);
  const RendererBitmap first = renderer.takeSnapshot();
  renderer.draw(*originalDocument);
  editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), first,
                                       "reused_filter_slots_unchanged",
                                       editor::tests::PixelmatchIdentityParams());
  const geode::GeodeFilterEngine::CachedBindingCounts settled =
      device->filterEngine().cachedBindingCountsForTesting();

  renderer.draw(*editedDocument);
  editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), RenderOnFreshDevice(edited),
                                       "reused_filter_slots_edited",
                                       editor::tests::PixelmatchIdentityParams());
  // Premise: the edit built no new binding, so only the uploads can carry it.
  const geode::GeodeFilterEngine::CachedBindingCounts afterEdit =
      device->filterEngine().cachedBindingCountsForTesting();
  EXPECT_EQ(afterEdit.textureViews, settled.textureViews);
  EXPECT_EQ(afterEdit.bindGroups, settled.bindGroups);
  EXPECT_THAT(device->isDeviceLost(), testing::IsFalse());
}

// Cached filter views and bind groups are found by the identities of the textures and buffers they
// name. A frame that hands the same textures to other passes, and a frame whose textures were
// destroyed and replaced, possibly in the same runtime slots, must each render exactly what a
// device with nothing cached renders, and entries naming destroyed textures must not outlive the
// next frame boundary.
TEST_F(RendererGeodeTest, CachedFilterBindingsFollowTextureReuseAndReplacement) {
  const std::string blurred = FilteredShapesSvg(
      R"(<feGaussianBlur stdDeviation="2" result="soft"/>
         <feOffset dx="3" dy="2"/>
         <feComposite in2="soft" operator="arithmetic" k2="0.6" k3="0.6"/>)");
  const std::string morphed = FilteredShapesSvg(
      R"(<feMorphology operator="dilate" radius="2" result="thick"/>
         <feColorMatrix type="hueRotate" values="90"/>
         <feMerge><feMergeNode in="thick"/><feMergeNode/></feMerge>)");
  std::optional<SVGDocument> blurredDocument = ParseFilterDocument(blurred);
  std::optional<SVGDocument> morphedDocument = ParseFilterDocument(morphed);
  ASSERT_THAT(blurredDocument, testing::Optional(testing::_));
  ASSERT_THAT(morphedDocument, testing::Optional(testing::_));
  const RendererBitmap blurredReference = RenderOnFreshDevice(blurred);
  const RendererBitmap morphedReference = RenderOnFreshDevice(morphed);

  const std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
  ASSERT_THAT(device, testing::NotNull());
  RendererGeode renderer(device);
  const auto drawAndCompare = [&](SVGDocument& document, const RendererBitmap& reference,
                                  std::string_view name) {
    SCOPED_TRACE(name);
    renderer.draw(document);
    editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), reference,
                                         "cached_filter_bindings_" + std::string(name),
                                         editor::tests::PixelmatchIdentityParams());
    EXPECT_THAT(device->isDeviceLost(), testing::IsFalse());
  };

  drawAndCompare(*blurredDocument, blurredReference, "blurred_first");
  const geode::GeodeFilterEngine::CachedBindingCounts blurredOnly =
      device->filterEngine().cachedBindingCountsForTesting();
  ASSERT_GT(blurredOnly.bindGroups, 0u);

  // The other graph takes the same pooled textures in different roles, then the first takes them
  // back.
  drawAndCompare(*morphedDocument, morphedReference, "morphed_on_reused_textures");
  drawAndCompare(*blurredDocument, blurredReference, "blurred_after_reuse");

  // Destroying every pooled texture makes the next frame create new ones, which the runtime may
  // place in the slots just freed.
  renderer.trimTexturePool(RendererGeodeTexturePoolTrim::All);
  drawAndCompare(*blurredDocument, blurredReference, "blurred_on_replaced_textures");
  drawAndCompare(*blurredDocument, blurredReference, "blurred_settled");

  // Only the last frame's bindings remain: the ones naming destroyed or unused textures are gone.
  const geode::GeodeFilterEngine::CachedBindingCounts settled =
      device->filterEngine().cachedBindingCountsForTesting();
  EXPECT_EQ(settled.textureViews, blurredOnly.textureViews);
  EXPECT_EQ(settled.bindGroups, blurredOnly.bindGroups);
}

TEST_F(RendererGeodeTest, SettledFilterFramesReuseParameterScratch) {
  for (const bool replaceOpenFrame : {false, true}) {
    SCOPED_TRACE(replaceOpenFrame);
    const std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
    ASSERT_THAT(device, testing::NotNull());
    RendererGeode renderer(device);
    if (replaceOpenFrame) {
      beginFrame(renderer);
      renderer = RendererGeode(device);
      EXPECT_EQ(device->oldestOpenFrameGeneration(), std::numeric_limits<uint64_t>::max());
    }
    components::FilterGraph graph;
    graph.colorInterpolationFilters = ColorInterpolationFilters::SRGB;
    for (size_t index = 0; index < components::kMaximumFilterGraphNodes; ++index) {
      components::FilterNode node;
      node.primitive =
          components::filter_primitive::Flood{.floodColor = css::Color(css::RGBA(255, 0, 0, 255))};
      graph.nodes.push_back(node);
    }
    for (int frame = 0; frame < 16; ++frame) {
      SCOPED_TRACE(frame);
      beginFrame(renderer);
      renderer.pushFilterLayer(graph, Box2d({0, 0}, {kViewportSize, kViewportSize}));
      renderer.popFilterLayer();
      renderer.endFrame();
      const RendererBitmap pixels = renderer.takeSnapshot();
      ASSERT_THAT(pixels.dimensions, testing::Eq(Vector2i(kViewportSize, kViewportSize)));
      EXPECT_THAT(PixelAt(pixels, 32, 32), Rgba(255, 0, 0, 255));
      if (frame >= 2) {
        EXPECT_EQ(renderer.lastFrameTimings().counters.bufferCreates, 0u);
      }
    }
  }
}

TEST_F(RendererGeodeTest, OverlappingFiltersPreserveEachFramesParameters) {
  for (const bool parentFirst : {false, true}) {
    SCOPED_TRACE(parentFirst);
    const std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
    ASSERT_THAT(device, testing::NotNull());
    RendererGeode parent(device);
    RendererGeode sibling(device);
    const auto drawFlood = [](RendererGeode& renderer, css::RGBA color) {
      components::FilterGraph graph;
      graph.colorInterpolationFilters = ColorInterpolationFilters::SRGB;
      components::FilterNode node;
      node.primitive = components::filter_primitive::Flood{.floodColor = css::Color(color)};
      graph.nodes.push_back(node);
      renderer.pushFilterLayer(graph, Box2d({0, 0}, {kViewportSize, kViewportSize}));
      renderer.popFilterLayer();
    };
    beginFrame(parent);
    drawFlood(parent, css::RGBA(255, 0, 0, 255));
    beginFrame(sibling);
    drawFlood(sibling, css::RGBA(0, 0, 255, 255));
    if (parentFirst) {
      parent.endFrame();
      sibling.endFrame();
    } else {
      sibling.endFrame();
      parent.endFrame();
    }
    const RendererBitmap parentPixels = parent.takeSnapshot();
    const RendererBitmap siblingPixels = sibling.takeSnapshot();
    ASSERT_THAT(parentPixels.dimensions, testing::Eq(Vector2i(kViewportSize, kViewportSize)));
    ASSERT_THAT(siblingPixels.dimensions, testing::Eq(parentPixels.dimensions));
    for (const int coordinate : {1, 32, 62}) {
      SCOPED_TRACE(coordinate);
      EXPECT_THAT(PixelAt(parentPixels, coordinate, coordinate), Rgba(255, 0, 0, 255));
      EXPECT_THAT(PixelAt(siblingPixels, coordinate, coordinate), Rgba(0, 0, 255, 255));
    }
  }
}

TEST_F(RendererGeodeTest, OverlappingFramesKeepReplayWithTheirOwner) {
  const std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
  ASSERT_THAT(device, testing::NotNull());
  RendererGeode parent(device);
  RendererGeode sibling(device);
  beginFrame(parent);
  parent.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  parent.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});
  beginFrame(sibling);
  sibling.setPaint(solidFill(css::RGBA(0, 0, 255, 255)));
  sibling.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});
  parent.endFrame();
  const RendererBitmap parentPixels = parent.takeSnapshot();
  ASSERT_THAT(parentPixels.dimensions, testing::Eq(Vector2i(kViewportSize, kViewportSize)));
  EXPECT_THAT(PixelAt(parentPixels, 32, 32), Rgba(255, 0, 0, 255));
  sibling.endFrame();
  const RendererBitmap siblingPixels = sibling.takeSnapshot();
  ASSERT_THAT(siblingPixels.dimensions, testing::Eq(parentPixels.dimensions));
  EXPECT_THAT(PixelAt(siblingPixels, 32, 32), Rgba(0, 0, 255, 255));
}

TEST_F(RendererGeodeTest, AbandoningOneFramePreservesItsUnsubmittedSibling) {
  for (const bool destroy : {false, true}) {
    SCOPED_TRACE(destroy);
    const std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
    ASSERT_THAT(device, testing::NotNull());
    auto parent = std::make_unique<RendererGeode>(device);
    RendererGeode sibling(device);
    const auto drawFlood = [](RendererGeode& renderer, css::RGBA color) {
      components::FilterGraph graph;
      graph.colorInterpolationFilters = ColorInterpolationFilters::SRGB;
      components::FilterNode node;
      node.primitive = components::filter_primitive::Flood{.floodColor = css::Color(color)};
      graph.nodes.push_back(node);
      renderer.pushFilterLayer(graph, Box2d({0, 0}, {kViewportSize, kViewportSize}));
      renderer.popFilterLayer();
    };
    struct PendingHostCopy {
      gpu::Texture source;
      gpu::Texture destination;
      uint64_t serial = 0;
    };
    const auto registerPendingHostCopy = [&]() {
      gpu::Device& adapter = device->runtimeDevice();
      PendingHostCopy pending{
          .source = gpu::GetResultOrFail(adapter.createTexture(
              gpu::TextureDescriptor{"pending host copy source", gpu::Extent2d{1, 1},
                                     gpu::TextureFormat::RGBA8Unorm, gpu::TextureUsage::CopySrc})),
          .destination = gpu::GetResultOrFail(adapter.createTexture(
              gpu::TextureDescriptor{"pending host copy destination", gpu::Extent2d{1, 1},
                                     gpu::TextureFormat::RGBA8Unorm, gpu::TextureUsage::CopyDst})),
      };
      std::unique_ptr<gpu::CommandEncoder> commands =
          gpu::GetResultOrFail(adapter.createCommandEncoder());
      EXPECT_THAT(
          commands->copyTextureToTexture(pending.source, pending.destination, gpu::Extent2d{1, 1}),
          gpu::IsOk());
      pending.serial =
          gpu::GetResultOrFail(adapter.submit(gpu::GetResultOrFail(commands->finish())));
      return pending;
    };
    beginFrame(*parent);
    drawFlood(*parent, css::RGBA(255, 0, 0, 255));
    const PendingHostCopy parentPending = registerPendingHostCopy();
    beginFrame(sibling);
    drawFlood(sibling, css::RGBA(0, 0, 255, 255));
    const PendingHostCopy siblingPending = registerPendingHostCopy();
    ASSERT_THAT(siblingPending.serial, testing::Gt(parentPending.serial));
    // Neither frame has reached the queue, so nothing either of them recorded can have taken a
    // serial of its own between the two copies above.
    ASSERT_THAT(siblingPending.serial, testing::Eq(parentPending.serial + 1));

    if (destroy) {
      parent.reset();
    } else {
      beginFrame(*parent);
    }

    ASSERT_THAT(device->runtimeDevice().waitForSerial(siblingPending.serial, 2.0),
                testing::IsTrue());
    sibling.endFrame();
    const uint64_t siblingFrameSerial = device->runtimeDevice().lastSubmittedSerial();
    ASSERT_THAT(siblingFrameSerial, testing::Gt(siblingPending.serial));
    ASSERT_THAT(device->runtimeDevice().waitForSerial(siblingFrameSerial, 2.0), testing::IsTrue());
    const RendererBitmap pixels = sibling.takeSnapshot();
    ASSERT_THAT(pixels.dimensions, testing::Eq(Vector2i(kViewportSize, kViewportSize)));
    EXPECT_THAT(PixelAt(pixels, 32, 32), Rgba(0, 0, 255, 255));
  }
}

/// Smoke test: empty frame should snap to a fully transparent bitmap.
TEST_F(RendererGeodeTest, EmptyFrameIsTransparent) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);
  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  ASSERT_FALSE(snap.empty());
  EXPECT_EQ(snap.dimensions.x, static_cast<int>(kViewportSize));
  EXPECT_EQ(snap.dimensions.y, static_cast<int>(kViewportSize));

  auto pixel = PixelAt(snap, 32, 32);
  EXPECT_THAT(pixel, IsTransparent()) << "Empty frame should be transparent";
}

TEST_F(RendererGeodeTest, PreCancelledInterruptibleSnapshotSkipsGpuReadback) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);
  renderer.endFrame();
  (void)renderer.consumeReadbackStats();
  const std::uint64_t buffersBefore = sharedDevice()->lifetimeBufferCreates();

  const RendererBitmap snapshot = renderer.takeSnapshotInterruptibly([] { return true; });

  EXPECT_TRUE(snapshot.empty());
  EXPECT_EQ(sharedDevice()->lifetimeBufferCreates(), buffersBefore)
      << "A pre-cancelled low-priority snapshot must not allocate a GPU readback buffer";
  const RendererReadbackStats stats = renderer.consumeReadbackStats();
  EXPECT_EQ(stats.count, 0);
  EXPECT_EQ(stats.pollIterations, 0);
}

/// A device lost AFTER the map is requested is a different route from one lost before it, and
/// moving the wait onto the runtime created it: a wait slice checks the lost flag itself and
/// reports the loss within about one slice, where the readback previously discovered it only
/// when the ten-second deadline expired and declared a timeout.
///
/// Worth pinning in both directions. The readback must give up promptly, and it must NOT record
/// a timeout attribution while doing so - that attribution says "a deadline discovered this",
/// and here the driver did. The statistics must still be recorded, because they are taken
/// before the mapping handle is consumed; a wrapper that unmapped first would have nothing left
/// to read them from.
///
/// A fresh device, not the suite's shared one, so the injected loss cannot poison other cases.
TEST_F(RendererGeodeTest, DeviceLostWhileTheMapIsPendingIsReportedWithinASlice) {
  std::shared_ptr<geode::GeodeDevice> device(geode::GeodeDevice::CreateHeadless());
  ASSERT_NE(device, nullptr);

  RendererGeode renderer(device);
  beginFrame(renderer);
  renderer.endFrame();
  (void)renderer.consumeReadbackStats();

  bool mapRequested = false;
  device->setSnapshotReadbackHookForTesting([&](geode::GeodeDevice::SnapshotReadbackPhase phase) {
    if (phase == geode::GeodeDevice::SnapshotReadbackPhase::MapRequested) {
      mapRequested = true;
      device->markDeviceLost("test-injected loss while the map was pending");
    }
  });
  const auto start = std::chrono::steady_clock::now();
  const RendererBitmap snapshot = renderer.takeSnapshot();
  device->setSnapshotReadbackHookForTesting({});
  const auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_TRUE(snapshot.empty());
  EXPECT_THAT(mapRequested, testing::IsTrue());
  // Well under the ten-second readback deadline: the slice reports the loss rather than the
  // deadline discovering it.
  EXPECT_LT(elapsed, std::chrono::seconds(2));

  const RendererReadbackStats stats = renderer.consumeReadbackStats();
  EXPECT_EQ(stats.count, 1) << "the readback statistics are recorded before the mapping handle "
                               "is consumed, so an abandoned map still reports";
  EXPECT_GE(stats.pollIterations, 1);
  EXPECT_TRUE(stats.deviceLost);
  EXPECT_EQ(stats.timedOutWaitSite, GpuWaitTimeoutSite::None)
      << "a driver-reported loss must not be attributed to a wait deadline";

  // The abandoned readback leaves its buffer for the lost device to take with it rather than
  // destroying it, so running the whole path again must neither crash nor double-free. On the
  // sanitizer lanes this is where a double destroy would surface.
  EXPECT_TRUE(renderer.takeSnapshot().empty());
}

TEST_F(RendererGeodeTest, InterruptibleSnapshotCancelsPromptlyAfterGpuSubmit) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);
  renderer.endFrame();
  (void)renderer.consumeReadbackStats();

  std::atomic<bool> cancel{false};
  sharedDevice()->setSnapshotReadbackHookForTesting(
      [&](geode::GeodeDevice::SnapshotReadbackPhase phase) {
        if (phase == geode::GeodeDevice::SnapshotReadbackPhase::MapRequested) {
          cancel.store(true, std::memory_order_relaxed);
        }
      });
  const auto start = std::chrono::steady_clock::now();
  const RendererBitmap snapshot =
      renderer.takeSnapshotInterruptibly([&] { return cancel.load(std::memory_order_relaxed); });
  sharedDevice()->setSnapshotReadbackHookForTesting({});
  const auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_TRUE(snapshot.empty());
  EXPECT_LT(elapsed, std::chrono::milliseconds(250))
      << "The sole render worker must not remain blocked in thumbnail map/readback";
  const RendererReadbackStats stats = renderer.consumeReadbackStats();
  EXPECT_EQ(stats.count, 1);
  EXPECT_EQ(stats.pollIterations, 0)
      << "Cancellation should be observed before entering the GPU poll loop";
}

/// A capture cancelled after its readback reached the queue still names the source texture on its
/// capture context until that readback completes. Once it has, the owner must find nothing else
/// holding the texture when it releases it: no bytes stay resident on the owner's behalf, and its
/// runtime reports giving up ownership of the released target.
///
/// The capture context submits to its own queue on a native backend, so the owner's queue going
/// idle does not mean the capture's readback has. The test waits for the capture queue itself,
/// then runs the owner's release point once.
TEST_F(RendererGeodeTest, ACancelledCaptureStopsHoldingItsSourceOnceItsWorkCompletes) {
  std::shared_ptr<geode::GeodeDevice> device(geode::GeodeDevice::CreateHeadless());
  ASSERT_NE(device, nullptr);
  RendererGeode renderer(device);
  beginFrame(renderer);
  renderer.endFrame();

  std::atomic<bool> cancel{false};
  device->setSnapshotReadbackHookForTesting([&](geode::GeodeDevice::SnapshotReadbackPhase phase) {
    if (phase == geode::GeodeDevice::SnapshotReadbackPhase::MapRequested) {
      cancel.store(true, std::memory_order_relaxed);
    }
  });
  EXPECT_TRUE(
      renderer.takeSnapshotInterruptibly([&] { return cancel.load(std::memory_order_relaxed); })
          .empty());
  device->setSnapshotReadbackHookForTesting({});

  const std::uint64_t releasesBefore = device->lifetimeTextureReleases();
  std::shared_ptr<const RendererTextureSnapshot> snapshot = renderer.takeTextureSnapshot();
  ASSERT_NE(snapshot, nullptr);
  snapshot.reset();
  ASSERT_EQ(device->waitForQueueIdle(), geode::GpuWaitResult::Complete);
  ASSERT_EQ(device->waitForSnapshotCaptureIdleForTesting(), geode::GpuWaitResult::Complete);
  device->drainDeferredTextureBackings();

  EXPECT_EQ(renderer.consumeReadbackStats().sharedTextureTailBytes, 0u)
      << "no bytes may stay resident on the owner's behalf once every reader has finished";
  EXPECT_EQ(device->lifetimeTextureReleases(), releasesBefore + 1u)
      << "the cancelled capture's registration must not keep the released target's backing "
         "owned after its readback completed";
}

/// The readback context can finish its lease after the editor's last frame. While the lease is
/// active the owner must schedule idle maintenance without blocking on it; when cancellation
/// releases the lease it must wake that owner, which then polls the capture context without
/// requiring another draw or submit.
TEST_F(RendererGeodeTest, CancelledCaptureWakesIdleOwnerWithoutAnotherFrame) {
  std::shared_ptr<geode::GeodeDevice> device(geode::GeodeDevice::CreateHeadless());
  ASSERT_NE(device, nullptr);
  RendererGeode renderer(device);
  beginFrame(renderer);
  renderer.endFrame();
  ASSERT_EQ(device->waitForQueueIdle(), geode::GpuWaitResult::Complete);
  device->pollIdle();
  ASSERT_FALSE(device->hasIdleWork());

  std::atomic<int> wakes{0};
  device->setIdleWakeCallback([&] { wakes.fetch_add(1, std::memory_order_release); });
#if defined(__APPLE__)
  gpu::metal::MetalDevice* captureBackend = nullptr;
  device->setSnapshotCaptureRuntimeHookForTesting([&](gpu::Device& runtime) {
    captureBackend = &static_cast<gpu::metal::MetalDevice&>(runtime);
    captureBackend->holdNextCompletionForTest();
  });
#endif
  std::atomic<bool> cancel{false};
  std::promise<void> mapReached;
  std::future<void> atMap = mapReached.get_future();
  std::latch resumeCapture(1);
  device->setSnapshotReadbackHookForTesting([&](geode::GeodeDevice::SnapshotReadbackPhase phase) {
    if (phase == geode::GeodeDevice::SnapshotReadbackPhase::MapRequested) {
      cancel.store(true, std::memory_order_release);
      mapReached.set_value();
      resumeCapture.wait();
    }
  });

  RendererBitmap captured;
  std::thread reader([&] {
    captured =
        renderer.takeSnapshotInterruptibly([&] { return cancel.load(std::memory_order_acquire); });
  });
  if (atMap.wait_for(std::chrono::seconds(8)) != std::future_status::ready) {
    cancel.store(true, std::memory_order_release);
    resumeCapture.count_down();
    reader.join();
    device->setSnapshotReadbackHookForTesting({});
#if defined(__APPLE__)
    if (captureBackend) {
      captureBackend->releaseHeldCompletionForTest();
    }
    device->setSnapshotCaptureRuntimeHookForTesting({});
#endif
    device->setIdleWakeCallback({});
    FAIL() << "capture did not reach its in-flight readback map";
  }

  const auto pollStart = std::chrono::steady_clock::now();
  EXPECT_TRUE(device->hasIdleWork()) << "an active capture needs an idle owner wake";
  device->pollIdle();
  EXPECT_LT(std::chrono::steady_clock::now() - pollStart, std::chrono::seconds(1))
      << "an idle poll must not wait for a capture lease held on another thread";

  resumeCapture.count_down();
  reader.join();
  device->setSnapshotReadbackHookForTesting({});
  EXPECT_TRUE(captured.empty());
  EXPECT_GT(wakes.load(std::memory_order_acquire), 0)
      << "releasing the cancelled capture must wake the idle owner";
#if defined(__APPLE__)
  EXPECT_NE(captureBackend, nullptr);
  EXPECT_TRUE(device->hasIdleWork())
      << "the capture's held readback submission still has resources to retire";
  if (captureBackend) {
    captureBackend->releaseHeldCompletionForTest();
  }
  device->setSnapshotCaptureRuntimeHookForTesting({});
#endif
  ASSERT_EQ(device->waitForSnapshotCaptureIdleForTesting(), geode::GpuWaitResult::Complete);
  device->pollIdle();
  EXPECT_FALSE(device->hasIdleWork()) << "idle maintenance must reclaim capture work";
  device->setIdleWakeCallback({});
}

TEST_F(RendererGeodeTest, EmptyFrameAfterOpaqueFrameClearsReusedTarget) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);
  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  renderer.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});
  renderer.endFrame();

  RendererBitmap opaqueSnap = renderer.takeSnapshot();
  ASSERT_FALSE(opaqueSnap.empty());
  EXPECT_THAT(PixelAt(opaqueSnap, 32, 32), RgbaEq(255, 0, 0, 255));

  beginFrame(renderer);
  renderer.endFrame();

  RendererBitmap transparentSnap = renderer.takeSnapshot();
  ASSERT_FALSE(transparentSnap.empty());
  EXPECT_THAT(PixelAt(transparentSnap, 32, 32), IsTransparent())
      << "A same-size Geode frame with no draws must clear pixels from the previous frame.";
}

// ---------------------------------------------------------------------------
// Transparency checkerboard (`geode::GeodeCheckerboardPass`)
//
// A GPU presentation surface that receives one already-composed texture cannot
// draw the checkerboard *before* the document, so its checkerboard has to go in
// destination-over after the compose instead. These pin that pass: without it,
// every see-through document pixel reaches the presenting compositor as alpha
// zero and whatever sits behind the surface shows where checkerboard belongs.
// ---------------------------------------------------------------------------

/// Light-cell channel value, as an 8-bit sRGB level.
constexpr int kCheckerLight =
    static_cast<int>(geode::kTransparencyCheckerboardLightColor[0] * 255.0f + 0.5f);
/// Dark-cell channel value, as an 8-bit sRGB level.
constexpr int kCheckerDark =
    static_cast<int>(geode::kTransparencyCheckerboardDarkColor[0] * 255.0f + 0.5f);
/// Cell size in logical pixels, as an integer pixel step for sampling.
constexpr int kCheckerCell = static_cast<int>(geode::kTransparencyCheckerboardCellLogicalPx);

TEST_F(RendererGeodeTest, CheckerboardFillsTransparentPixelsWithAlternatingCells) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);
  renderer.endFrame();

  const std::shared_ptr<const RendererTextureSnapshot> frame = takeFinishedFrame(renderer);
  ASSERT_NE(frame, nullptr);
  ASSERT_TRUE(drawCheckerboard(*frame, geode::CheckerboardUnderlayParams{}));
  const uint64_t checkerboardSerial = sharedDevice()->runtimeDevice().lastSubmittedSerial();

  const RendererBitmap snapshot = frame->takeSnapshot();
  ASSERT_FALSE(snapshot.empty());
  EXPECT_THAT(sharedDevice()->runtimeDevice().completedSerial(), testing::Ge(checkerboardSerial))
      << "Readback completion must cover the checkerboard submission while the frame owner keeps "
         "the borrowed target backing alive";
  const auto& geodeFrame = static_cast<const RendererGeodeTextureSnapshot&>(*frame);
  ASSERT_THAT(geodeFrame.runtimeTexture(), testing::NotNull());
  EXPECT_THAT(sharedDevice()->runtimeDevice().textureDescriptor(*geodeFrame.runtimeTexture()),
              gpu::HasResult())
      << "Completing a pass over a borrowed target must not retire its owner-held backing";
  // Two horizontally adjacent cells: (0,0) is light, (1,0) is dark.
  EXPECT_THAT(PixelAt(snapshot, kCheckerCell / 2, kCheckerCell / 2),
              RgbaEq(kCheckerLight, kCheckerLight, kCheckerLight, 255))
      << "Cell (0,0) of a fully transparent frame must read as the light checker color";
  EXPECT_THAT(PixelAt(snapshot, kCheckerCell + kCheckerCell / 2, kCheckerCell / 2),
              RgbaEq(kCheckerDark, kCheckerDark, kCheckerDark, 255))
      << "The horizontally adjacent cell must read as the dark checker color";
  // And vertically, so a solid fill of either color cannot pass.
  EXPECT_THAT(PixelAt(snapshot, kCheckerCell / 2, kCheckerCell + kCheckerCell / 2),
              RgbaEq(kCheckerDark, kCheckerDark, kCheckerDark, 255))
      << "The vertically adjacent cell must read as the dark checker color";
}

TEST_F(RendererGeodeTest, CheckerboardLeavesOpaqueContentUntouched) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);
  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  renderer.drawRect(Box2d({32, 32}, {kViewportSize, kViewportSize}), StrokeParams{});
  renderer.endFrame();

  const std::shared_ptr<const RendererTextureSnapshot> frame = takeFinishedFrame(renderer);
  ASSERT_NE(frame, nullptr);
  ASSERT_TRUE(drawCheckerboard(*frame, geode::CheckerboardUnderlayParams{}));

  const RendererBitmap snapshot = frame->takeSnapshot();
  ASSERT_FALSE(snapshot.empty());
  EXPECT_THAT(PixelAt(snapshot, 48, 48), RgbaEq(255, 0, 0, 255))
      << "Destination-over must not touch fully opaque document pixels";
  EXPECT_THAT(PixelAt(snapshot, kCheckerCell / 2, kCheckerCell / 2),
              RgbaEq(kCheckerLight, kCheckerLight, kCheckerLight, 255))
      << "Transparent pixels beside the document content must become checkerboard";
}

/// A snapshot is routinely asked for while another renderer's frame is open - a thumbnail or an
/// async render of an earlier frame, taken while the main document is mid-frame - and it has its
/// own map to wait on. Its work therefore has to reach the queue rather than being appended to
/// that frame's command buffer, which would leave it unsubmitted until the frame ends and the
/// readback waiting out its deadline for a map of work that never ran.
///
/// The content assertion is what makes this discriminating. A spliced readback still maps its
/// buffer successfully and returns a full bitmap - of zeros, because the copy never executed -
/// so a snapshot checked only for "not empty", or for transparent pixels, passes either way.
TEST_F(RendererGeodeTest, SnapshotReachesTheQueueWhileAnotherFrameOwnsTheCommandStream) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);
  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  renderer.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});
  renderer.endFrame();

  const std::shared_ptr<const RendererTextureSnapshot> frame = takeFinishedFrame(renderer);
  ASSERT_NE(frame, nullptr);

  // From here until its endFrame, this second renderer owns the device's command stream.
  RendererGeode other = createRenderer();
  beginFrame(other);

  const RendererBitmap duringOtherFrame = frame->takeSnapshot();
  ASSERT_FALSE(duringOtherFrame.empty());
  EXPECT_THAT(PixelAt(duringOtherFrame, kViewportSize / 2, kViewportSize / 2),
              RgbaEq(255, 0, 0, 255))
      << "The snapshot must carry the content submitted before it was asked for, which it can "
         "only do if its own work reached the queue instead of the open frame's command buffer";

  other.endFrame();
}

TEST_F(RendererGeodeTest, CheckerboardBlendsUnderPartialAlpha) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);
  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 128)));
  renderer.drawRect(Box2d({32, 32}, {kViewportSize, kViewportSize}), StrokeParams{});
  renderer.endFrame();

  const std::shared_ptr<const RendererTextureSnapshot> frame = takeFinishedFrame(renderer);
  ASSERT_NE(frame, nullptr);
  ASSERT_TRUE(drawCheckerboard(*frame, geode::CheckerboardUnderlayParams{}));

  const RendererBitmap snapshot = frame->takeSnapshot();
  ASSERT_FALSE(snapshot.empty());
  // (48,48) is cell (3,3), a light cell. Premultiplied red over it:
  // r = 128 + 60 * (1 - 128/255) ~= 158, g = b = 60 * (1 - 128/255) ~= 30.
  constexpr int kCoverage = 128;
  const double transmitted = 1.0 - static_cast<double>(kCoverage) / 255.0;
  const int expectedRed = static_cast<int>(kCoverage + kCheckerLight * transmitted + 0.5);
  const int expectedGreenBlue = static_cast<int>(kCheckerLight * transmitted + 0.5);
  EXPECT_THAT(PixelAt(snapshot, 48, 48), Rgba(Near(expectedRed, 2), Near(expectedGreenBlue, 2),
                                              Near(expectedGreenBlue, 2), Near(255, 1)))
      << "Half-covered document pixels must show the checkerboard through their remaining alpha";
}

TEST_F(RendererGeodeTest, CheckerboardOriginOffsetShiftsTheAnchor) {
  const auto sampleFirstTwoCells = [&](Vector2d originOffsetPx) {
    RendererGeode renderer = createRenderer();
    beginFrame(renderer);
    renderer.endFrame();
    const std::shared_ptr<const RendererTextureSnapshot> frame = takeFinishedFrame(renderer);
    EXPECT_NE(frame, nullptr);
    geode::CheckerboardUnderlayParams params;
    params.originOffsetPx = originOffsetPx;
    EXPECT_TRUE(drawCheckerboard(*frame, params));
    const RendererBitmap snapshot = frame->takeSnapshot();
    EXPECT_FALSE(snapshot.empty());
    return std::pair(PixelAt(snapshot, kCheckerCell / 2, kCheckerCell / 2),
                     PixelAt(snapshot, kCheckerCell + kCheckerCell / 2, kCheckerCell / 2));
  };

  // A surface that pans with the document anchors the pattern by its on-screen
  // position rather than by the texture origin. One cell of positive offset
  // must invert the two leading cells...
  const auto shifted = sampleFirstTwoCells(Vector2d(kCheckerCell, 0.0));
  EXPECT_THAT(shifted.first, RgbaEq(kCheckerDark, kCheckerDark, kCheckerDark, 255));
  EXPECT_THAT(shifted.second, RgbaEq(kCheckerLight, kCheckerLight, kCheckerLight, 255));

  // ...and so must one cell of negative offset, which is the case that produces
  // negative cell indices. Sign-preserving `%` would leave both cells dark.
  const auto shiftedNegative = sampleFirstTwoCells(Vector2d(-kCheckerCell, 0.0));
  EXPECT_THAT(shiftedNegative.first, RgbaEq(kCheckerDark, kCheckerDark, kCheckerDark, 255));
  EXPECT_THAT(shiftedNegative.second, RgbaEq(kCheckerLight, kCheckerLight, kCheckerLight, 255));
}

TEST_F(RendererGeodeTest, CheckerboardReachesItsTargetWhileAnotherFrameIsOpen) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);
  renderer.endFrame();

  const std::shared_ptr<const RendererTextureSnapshot> frame = takeFinishedFrame(renderer);
  ASSERT_NE(frame, nullptr);

  // A second renderer sharing the device opens a frame. What that frame records accumulates in
  // command buffers of its own, so a submission asked for here is its own command buffer on the
  // queue rather than a span spliced into someone else's recording.
  RendererGeode other = createRenderer();
  beginFrame(other);

  ASSERT_TRUE(drawCheckerboard(*frame, geode::CheckerboardUnderlayParams{}));

  const RendererBitmap duringOtherFrame = frame->takeSnapshot();
  ASSERT_FALSE(duringOtherFrame.empty());
  EXPECT_THAT(PixelAt(duringOtherFrame, kCheckerCell / 2, kCheckerCell / 2),
              RgbaEq(kCheckerLight, kCheckerLight, kCheckerLight, 255))
      << "An open frame elsewhere on the device must not keep a checkerboard from its target";

  other.endFrame();

  // The same call after that frame closes behaves identically, so nothing about it depended on
  // the overlap.
  ASSERT_TRUE(drawCheckerboard(*frame, geode::CheckerboardUnderlayParams{}));
  const RendererBitmap afterOtherFrame = frame->takeSnapshot();
  ASSERT_FALSE(afterOtherFrame.empty());
  EXPECT_THAT(PixelAt(afterOtherFrame, kCheckerCell / 2, kCheckerCell / 2),
              RgbaEq(kCheckerLight, kCheckerLight, kCheckerLight, 255));
}

TEST_F(RendererGeodeTest, CheckerboardCellsAreLogicalPixelsNotDevicePixels) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);
  renderer.endFrame();

  const std::shared_ptr<const RendererTextureSnapshot> frame = takeFinishedFrame(renderer);
  ASSERT_NE(frame, nullptr);
  geode::CheckerboardUnderlayParams params;
  params.devicePixelRatio = 2.0;
  ASSERT_TRUE(drawCheckerboard(*frame, params));

  const RendererBitmap snapshot = frame->takeSnapshot();
  ASSERT_FALSE(snapshot.empty());
  // At 2x the first cell spans 32 device pixels, so the sample that was dark at
  // 1x is still inside cell (0,0).
  EXPECT_THAT(PixelAt(snapshot, kCheckerCell + kCheckerCell / 2, kCheckerCell / 2),
              RgbaEq(kCheckerLight, kCheckerLight, kCheckerLight, 255))
      << "Cells are sized in logical pixels, so a 2x ratio doubles their device-pixel extent";
  EXPECT_THAT(PixelAt(snapshot, 2 * kCheckerCell + kCheckerCell / 2, kCheckerCell / 2),
              RgbaEq(kCheckerDark, kCheckerDark, kCheckerDark, 255));
}

TEST_F(RendererGeodeTest, CheckerboardScissorConfinesThePassToItsRegion) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);
  renderer.endFrame();

  const std::shared_ptr<const RendererTextureSnapshot> frame = takeFinishedFrame(renderer);
  ASSERT_NE(frame, nullptr);
  geode::CheckerboardUnderlayParams params;
  // The editor clips the window checkerboard to the render pane this way, so
  // the chrome around the pane keeps whatever is already in the framebuffer.
  params.scissorPx = geode::CheckerboardScissorPx{
      .x = 0,
      .y = 0,
      .width = static_cast<std::uint32_t>(kCheckerCell),
      .height = static_cast<std::uint32_t>(kCheckerCell),
  };
  ASSERT_TRUE(drawCheckerboard(*frame, params));

  const RendererBitmap snapshot = frame->takeSnapshot();
  ASSERT_FALSE(snapshot.empty());
  EXPECT_THAT(PixelAt(snapshot, kCheckerCell / 2, kCheckerCell / 2),
              RgbaEq(kCheckerLight, kCheckerLight, kCheckerLight, 255))
      << "Pixels inside the scissor must take the checkerboard";
  EXPECT_THAT(PixelAt(snapshot, kCheckerCell + kCheckerCell / 2, kCheckerCell / 2), IsTransparent())
      << "Pixels outside the scissor must be left exactly as the frame composed them";
}

TEST_F(RendererGeodeTest, CheckerboardRejectsDegenerateParameters) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);
  renderer.endFrame();

  const std::shared_ptr<const RendererTextureSnapshot> frame = takeFinishedFrame(renderer);
  ASSERT_NE(frame, nullptr);

  geode::CheckerboardUnderlayParams zeroRatio;
  zeroRatio.devicePixelRatio = 0.0;
  EXPECT_FALSE(drawCheckerboard(*frame, zeroRatio));

  geode::CheckerboardUnderlayParams zeroCell;
  zeroCell.cellSizeLogicalPx = 0.0;
  EXPECT_FALSE(drawCheckerboard(*frame, zeroCell));

  geode::CheckerboardUnderlayParams emptyScissor;
  emptyScissor.scissorPx = geode::CheckerboardScissorPx{};
  EXPECT_FALSE(drawCheckerboard(*frame, emptyScissor));

  const RendererBitmap snapshot = frame->takeSnapshot();
  ASSERT_FALSE(snapshot.empty());
  EXPECT_THAT(PixelAt(snapshot, kCheckerCell / 2, kCheckerCell / 2), IsTransparent())
      << "A rejected checkerboard pass must leave the target untouched";
}

TEST_F(RendererGeodeTest, CheckerboardRejectsInvalidTargetsWithoutUniformWorkOrSubmission) {
  const auto expectRefusedWithoutWork = [&](const gpu::Texture& target,
                                            Vector2i extent = Vector2i(8, 8)) {
    geode::GeodeCounters counters;
    sharedDevice()->setCounters(&counters);
    geode::GeodeCheckerboardPass pass;
    EXPECT_FALSE(pass.draw(*sharedDevice(), target, extent, geode::CheckerboardUnderlayParams{},
                           geode::GeodeCheckerboardPipeline::BlendMode::Replace));
    sharedDevice()->setCounters(nullptr);
    EXPECT_THAT(counters.bufferCreates, testing::Eq(0u));
    EXPECT_THAT(counters.bindgroupCreates, testing::Eq(0u));
    EXPECT_THAT(counters.submits, testing::Eq(0u));
  };

  std::shared_ptr<geode::GeodeDevice> foreignDevice = geode::GeodeDevice::CreateHeadless();
  ASSERT_THAT(foreignDevice, testing::NotNull());
  gpu::Result<gpu::Texture> foreignResult = foreignDevice->runtimeDevice().createTexture(
      gpu::TextureDescriptor{"foreignCheckerboardTarget", gpu::Extent2d{8, 8},
                             gpu::TextureFormat::RGBA8Unorm, gpu::TextureUsage::RenderAttachment});
  ASSERT_FALSE(foreignResult.hasError());
  const gpu::Texture foreign = std::move(foreignResult).result();
  expectRefusedWithoutWork(foreign);

  gpu::Device& runtime = sharedDevice()->runtimeDevice();
  gpu::Result<gpu::Texture> staleResult = runtime.createTexture(
      gpu::TextureDescriptor{"staleCheckerboardTarget", gpu::Extent2d{8, 8},
                             gpu::TextureFormat::RGBA8Unorm, gpu::TextureUsage::RenderAttachment});
  ASSERT_FALSE(staleResult.hasError());
  gpu::Texture stale = std::move(staleResult).result();
  const gpu::Texture staleHandle =
      gpu::Texture::CreateForBackend(stale.slotIndex(), stale.generation(), stale.deviceId());
  ASSERT_FALSE(runtime.destroyTexture(std::move(stale)).hasError());
  expectRefusedWithoutWork(staleHandle);

  gpu::Result<gpu::Texture> wrongFormatResult = runtime.createTexture(
      gpu::TextureDescriptor{"wrongFormatCheckerboardTarget", gpu::Extent2d{8, 8},
                             gpu::TextureFormat::R8Unorm, gpu::TextureUsage::RenderAttachment});
  ASSERT_FALSE(wrongFormatResult.hasError());
  const gpu::Texture wrongFormat = std::move(wrongFormatResult).result();
  expectRefusedWithoutWork(wrongFormat);

  gpu::Result<gpu::Texture> wrongUsageResult = runtime.createTexture(
      gpu::TextureDescriptor{"wrongUsageCheckerboardTarget", gpu::Extent2d{8, 8},
                             gpu::TextureFormat::RGBA8Unorm, gpu::TextureUsage::CopySrc});
  ASSERT_FALSE(wrongUsageResult.hasError());
  const gpu::Texture wrongUsage = std::move(wrongUsageResult).result();
  expectRefusedWithoutWork(wrongUsage);

  gpu::Result<gpu::Texture> wrongExtentResult = runtime.createTexture(
      gpu::TextureDescriptor{"wrongExtentCheckerboardTarget", gpu::Extent2d{8, 8},
                             gpu::TextureFormat::RGBA8Unorm, gpu::TextureUsage::RenderAttachment});
  ASSERT_FALSE(wrongExtentResult.hasError());
  const gpu::Texture wrongExtent = std::move(wrongExtentResult).result();
  expectRefusedWithoutWork(wrongExtent, Vector2i(7, 8));
}

/// Width/height should reflect the viewport's device-pixel size after
/// `beginFrame`.
TEST_F(RendererGeodeTest, WidthHeightReflectViewport) {
  RendererGeode renderer = createRenderer();
  RenderViewport viewport;
  viewport.size = Vector2d(48, 32);
  viewport.devicePixelRatio = 2.0;
  renderer.beginFrame(viewport);
  EXPECT_EQ(renderer.width(), 96);
  EXPECT_EQ(renderer.height(), 64);
  renderer.endFrame();
}

TEST_F(RendererGeodeTest, TakeTextureSnapshotReturnsTextureAndDetachesTarget) {
  ASSERT_TRUE(sharedDevice() != nullptr);
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);
  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  renderer.drawRect(Box2d({4, 4}, {20, 20}), StrokeParams{});
  renderer.endFrame();

  std::shared_ptr<const RendererTextureSnapshot> texture = renderer.takeTextureSnapshot();
  ASSERT_TRUE(texture != nullptr);
  EXPECT_EQ(texture->backend(), RendererTextureSnapshotBackend::Geode);
  EXPECT_EQ(texture->dimensions(),
            Vector2i(static_cast<int>(kViewportSize), static_cast<int>(kViewportSize)));
  const auto* geodeTexture = static_cast<const RendererGeodeTextureSnapshot*>(texture.get());
  EXPECT_THAT(geodeTexture->runtimeTexture(), testing::NotNull());
  EXPECT_THAT(geodeTexture->runtimeFormat(), testing::Optional(gpu::TextureFormat::RGBA8Unorm));
  const RendererBitmap textureBitmap = texture->takeSnapshot();
  ASSERT_FALSE(textureBitmap.empty());
  EXPECT_THAT(PixelAt(textureBitmap, 8, 8), RgbaEq(255, 0, 0, 255));

  EXPECT_TRUE(renderer.takeSnapshot().empty()) << "Texture export detaches the internal target so "
                                                  "presentation cannot be overwritten by readback";

  beginFrame(renderer);
  renderer.endFrame();
  std::shared_ptr<const RendererTextureSnapshot> secondTexture = renderer.takeTextureSnapshot();
  ASSERT_TRUE(secondTexture != nullptr);
  EXPECT_EQ(secondTexture->dimensions(), texture->dimensions());
}

/// A released snapshot enters the owner-context retirement mailbox and gives up its native
/// allocation only when the owner drains that mailbox.
TEST_F(RendererGeodeTest, OwnedTextureSnapshotReleasesBackingWhenOwnerDrains) {
  const std::shared_ptr<geode::GeodeDevice> device = sharedDevice();
  ASSERT_THAT(device, testing::NotNull());
  RendererGeode renderer(device);
  beginFrame(renderer);
  renderer.endFrame();

  const std::uint64_t releasesBefore = device->lifetimeTextureReleases();
  std::shared_ptr<const RendererTextureSnapshot> snapshot = renderer.takeTextureSnapshot();
  ASSERT_NE(snapshot, nullptr);
  snapshot.reset();

  EXPECT_EQ(device->lifetimeTextureReleases(), releasesBefore);
  EXPECT_THAT(device->deferredTextureDestroyCountForTesting(), testing::Eq(1u));
  device->drainDeferredTextureBackings();
  EXPECT_EQ(device->lifetimeTextureReleases(), releasesBefore + 1u)
      << "The owner-context drain must give up released snapshot backing";
}

/// A borrowed frame view does not give up ownership of the reusable native target.
TEST_F(RendererGeodeTest, BorrowedTextureSnapshotKeepsBacking) {
  const std::shared_ptr<geode::GeodeDevice> device = sharedDevice();
  ASSERT_THAT(device, testing::NotNull());
  RendererGeode renderer(device);
  beginFrame(renderer);
  renderer.endFrame();

  const std::uint64_t releasesBefore = device->lifetimeTextureReleases();
  const RendererTextureSnapshot* snapshot = renderer.borrowTextureSnapshot();
  ASSERT_NE(snapshot, nullptr);

  beginFrame(renderer);
  renderer.endFrame();

  EXPECT_EQ(device->lifetimeTextureReleases(), releasesBefore)
      << "A frame-local borrowed snapshot must not release the renderer's reusable target";
}

TEST_F(RendererGeodeTest, SynchronousDirectPresentationReusesSameSizeRenderTarget) {
  ASSERT_TRUE(sharedDevice() != nullptr);
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);
  renderer.endFrame();
  ASSERT_TRUE(renderer.borrowTextureSnapshot() != nullptr);

  beginFrame(renderer);
  renderer.endFrame();
  ASSERT_TRUE(renderer.borrowTextureSnapshot() != nullptr);

  beginFrame(renderer);
  renderer.endFrame();
  ASSERT_TRUE(renderer.borrowTextureSnapshot() != nullptr);
  EXPECT_EQ(renderer.lastFrameTimings().counters.textureCreates, 0u)
      << "A same-size synchronous direct presentation must borrow the current texture instead of "
         "detaching the renderer's reusable full-canvas target.";
}

TEST_F(RendererGeodeTest, ResizingRetiresTheSupersededTargetBackingAtTheFrameBoundary) {
  ASSERT_TRUE(sharedDevice() != nullptr);
  sharedDevice()->drainDeferredDestroys();
  RendererGeode renderer = createRenderer();

  RenderViewport viewport;
  viewport.size = Vector2d(64.0, 64.0);
  viewport.devicePixelRatio = 1.0;
  renderer.beginFrame(viewport);
  renderer.endFrame();
  ASSERT_EQ(sharedDevice()->deferredTextureDestroyCountForTesting(), 0u);

  renderer.beginFrame(viewport);
  EXPECT_EQ(sharedDevice()->deferredTextureDestroyCountForTesting(), 0u)
      << "A same-size frame must retain the reusable primary target";
  renderer.endFrame();

  viewport.size = Vector2d(96.0, 64.0);
  renderer.beginFrame(viewport);
  EXPECT_EQ(sharedDevice()->deferredTextureDestroyCountForTesting(), 1u)
      << "A superseded primary target must be queued for an explicit backing destroy while the "
         "frame that replaced it is still recording, not destroyed under it";
  renderer.endFrame();
  EXPECT_EQ(sharedDevice()->deferredTextureDestroyCountForTesting(), 0u)
      << "That backing must be freed at the frame boundary, rather than having only its runtime "
         "slot released and its allocation left for the host runtime to collect";

  viewport.size = Vector2d(128.0, 64.0);
  renderer.beginFrame(viewport);
  EXPECT_EQ(sharedDevice()->deferredTextureDestroyCountForTesting(), 1u)
      << "Each resize must queue the newly superseded target in its turn";
  renderer.endFrame();
}

TEST_F(RendererGeodeTest, RepeatedPatternTilesOfOneSizeAllocateNoTexturesInSteadyState) {
  ASSERT_TRUE(sharedDevice() != nullptr);
  RendererGeode renderer = createRenderer();

  const auto renderPatternFrame = [&](int frameIndex) {
    SCOPED_TRACE(testing::Message() << "pattern frame " << frameIndex);
    beginFrame(renderer);
    ASSERT_TRUE(renderer.beginPatternTile(Box2d::FromXYWH(0.0, 0.0, 8.0, 8.0), Transform2d()))
        << "the tile must be admitted, otherwise a zero allocation count proves nothing";
    renderer.setPaint(solidFill(css::RGBA(0, 0, 255, 255)));
    renderer.drawRect(Box2d::FromXYWH(0.0, 0.0, 8.0, 8.0), StrokeParams{});
    renderer.endPatternTile(/*forStroke=*/false, std::nullopt);
    renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
    renderer.drawRect(Box2d::FromXYWH(0.0, 0.0, kViewportSize, kViewportSize), StrokeParams{});
    renderer.endFrame();
  };

  renderPatternFrame(1);
  renderPatternFrame(2);
  renderPatternFrame(3);
  EXPECT_EQ(renderer.lastFrameTimings().counters.textureCreates, 0u)
      << "A pattern tile of an already-seen extent must come back out of the transient texture "
         "pool instead of allocating a new texture every frame";
}

TEST_F(RendererGeodeTest, APatternPaintTheFrameNeverConsumedIsRetiredNotCarriedForward) {
  ASSERT_TRUE(sharedDevice() != nullptr);
  RendererGeode renderer = createRenderer();

  beginFrame(renderer);
  const std::size_t pooledBefore = renderer.texturePoolStats().textureCount;
  ASSERT_TRUE(renderer.beginPatternTile(Box2d::FromXYWH(0.0, 0.0, 8.0, 8.0), Transform2d()));
  renderer.setPaint(solidFill(css::RGBA(0, 0, 255, 255)));
  renderer.drawRect(Box2d::FromXYWH(0.0, 0.0, 8.0, 8.0), StrokeParams{});
  // No fill or stroke follows, so the promoted pattern paint is never consumed.
  renderer.endPatternTile(/*forStroke=*/false, std::nullopt);
  renderer.endFrame();

  EXPECT_EQ(renderer.texturePoolStats().textureCount, pooledBefore + 1u)
      << "An unconsumed pattern tile must go back to the transient texture pool at the frame "
         "boundary rather than being destroyed outside it";

  beginFrame(renderer);
  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  renderer.drawRect(Box2d::FromXYWH(0.0, 0.0, kViewportSize, kViewportSize), StrokeParams{});
  renderer.endFrame();

  const RendererBitmap bitmap = renderer.takeSnapshot();
  EXPECT_THAT(PixelAt(bitmap, 32, 32), Rgba(255, 0, 0, 255))
      << "A pattern paint left over from the previous frame names a tile this frame may already "
         "have recycled, so the next fill must use its own paint instead";
}

TEST_F(RendererGeodeTest, APatternTileLeftOpenAcrossTheFrameBoundaryIsRefusedNotPromoted) {
  ASSERT_TRUE(sharedDevice() != nullptr);
  RendererGeode renderer = createRenderer();

  beginFrame(renderer);
  const std::size_t pooledBefore = renderer.texturePoolStats().textureCount;
  ASSERT_TRUE(renderer.beginPatternTile(Box2d::FromXYWH(0.0, 0.0, 8.0, 8.0), Transform2d()));
  renderer.setPaint(solidFill(css::RGBA(0, 0, 255, 255)));
  renderer.drawRect(Box2d::FromXYWH(0.0, 0.0, 8.0, 8.0), StrokeParams{});
  // The matching endPatternTile never arrives before the frame ends.
  renderer.endFrame();

  EXPECT_EQ(renderer.texturePoolStats().textureCount, pooledBefore + 1u)
      << "A tile whose pattern was still open at the frame boundary must be handed back to the "
         "transient texture pool";

  beginFrame(renderer);
  renderer.endPatternTile(/*forStroke=*/false, std::nullopt);
  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  renderer.drawRect(Box2d::FromXYWH(0.0, 0.0, kViewportSize, kViewportSize), StrokeParams{});
  renderer.endFrame();

  const RendererBitmap bitmap = renderer.takeSnapshot();
  EXPECT_THAT(PixelAt(bitmap, 32, 32), Rgba(255, 0, 0, 255))
      << "Promoting a tile the previous frame recycled would sample whatever now occupies its "
         "slot, so the unmatched endPatternTile must be refused";
}

TEST_F(RendererGeodeTest, AFrameAbandonedWithATileOpenLeavesNothingOfItselfInTheNextFrame) {
  ASSERT_TRUE(sharedDevice() != nullptr);
  RendererGeode renderer = createRenderer();

  // An 8x8 tile under a 2x transform, which supersampling takes to a 4x raster scale, so the
  // scale left behind is distinguishable from the identity the next frame must start on.
  RenderViewport abandonedViewport;
  abandonedViewport.size = Vector2d(kViewportSize, kViewportSize);
  abandonedViewport.devicePixelRatio = 1.0;
  renderer.beginFrame(abandonedViewport);
  // A scissor small enough to exclude the pixel the next frame checks, so carrying this clip
  // forward would be visible rather than harmless.
  ResolvedClip abandonedClip;
  abandonedClip.clipRect = Box2d::FromXYWH(0.0, 0.0, 2.0, 2.0);
  renderer.pushClip(abandonedClip);
  ASSERT_TRUE(
      renderer.beginPatternTile(Box2d::FromXYWH(0.0, 0.0, 8.0, 8.0), Transform2d::Scale(2.0, 2.0)));
  renderer.setPaint(solidFill(css::RGBA(0, 0, 255, 255)));
  renderer.drawRect(Box2d::FromXYWH(0.0, 0.0, 8.0, 8.0), StrokeParams{});
  // Neither endPatternTile nor endFrame arrives; the next beginFrame inherits whatever is left.

  RenderViewport nextViewport;
  nextViewport.size = Vector2d(kViewportSize / 2.0, kViewportSize / 2.0);
  nextViewport.devicePixelRatio = 1.0;
  renderer.beginFrame(nextViewport);
  EXPECT_EQ(renderer.width(), static_cast<int>(kViewportSize / 2.0))
      << "The new frame's extent comes from its own viewport, not from the frame that was "
         "abandoned";
  EXPECT_EQ(renderer.height(), static_cast<int>(kViewportSize / 2.0));

  renderer.setTransform(Transform2d());
  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  renderer.drawRect(Box2d::FromXYWH(0.0, 0.0, 8.0, 8.0), StrokeParams{});
  renderer.endFrame();

  const RendererBitmap bitmap = renderer.takeSnapshot();
  EXPECT_THAT(PixelAt(bitmap, 4, 4), Rgba(255, 0, 0, 255))
      << "The abandoned frame's clip must not scissor this one";
  EXPECT_THAT(PixelAt(bitmap, 12, 12), IsTransparent())
      << "An abandoned tile still on the stack would scale this frame's transform by its raster "
         "scale - 4x here, a 2x transform supersampled 2x - painting the rect four times as wide";
}

TEST_F(RendererGeodeTest, QuarterTurnMaskRegionsKeepTheAxisAlignedSurfaceBudget) {
  struct Case {
    double angleRadians;
    Vector2i inside;
  };
  const std::array<Case, 4> cases = {{{0, {20, 20}},
                                      {MathConstants<double>::kHalfPi, {12, 20}},
                                      {MathConstants<double>::kPi, {12, 12}},
                                      {3 * MathConstants<double>::kHalfPi, {20, 12}}}};
  for (const auto& test : cases) {
    SCOPED_TRACE(test.angleRadians);
    RendererGeode renderer = createRenderer();
    renderer.setSurfaceBudgetForTesting(/*maximumSurfaces=*/3u, /*maximumBytes=*/1u << 20);
    beginFrame(renderer);
    renderer.setTransform(Transform2d::Rotate(test.angleRadians) *
                          Transform2d::Translate({16, 16}));
    renderer.pushMask(Box2d::FromXYWH(0, 0, 8, 8), MaskType::Alpha);
    EXPECT_THAT(renderer.resourceStats().surfaceBudgetRejected, testing::IsFalse());
    EXPECT_THAT(renderer.resourceStats().surfaceCount, testing::Eq(3u));
    renderer.setTransform(Transform2d());
    renderer.setPaint(solidFill(css::RGBA(255, 255, 255, 255)));
    renderer.drawRect(Box2d::FromXYWH(0, 0, kViewportSize, kViewportSize), StrokeParams{});
    renderer.transitionMaskToContent();
    renderer.setPaint(solidFill(css::RGBA(0, 128, 0, 255)));
    renderer.drawRect(Box2d::FromXYWH(0, 0, kViewportSize, kViewportSize), StrokeParams{});
    renderer.popMask();
    renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
    renderer.drawRect(Box2d::FromXYWH(48, 0, 8, kViewportSize), StrokeParams{});
    renderer.endFrame();
    const RendererBitmap bitmap = renderer.takeSnapshot();
    ASSERT_THAT(bitmap.empty(), testing::IsFalse());
    EXPECT_THAT(PixelAt(bitmap, test.inside.x, test.inside.y), RgbaEq(0, 128, 0, 255));
    EXPECT_THAT(PixelAt(bitmap, 2, 2), IsTransparent());
    EXPECT_THAT(PixelAt(bitmap, 52, 32), RgbaEq(255, 0, 0, 255));
  }
}

TEST_F(RendererGeodeTest, NearAxisMaskWithLargeExtentStillRequiresExactClipAdmission) {
  struct Case {
    double shear;
    double width;
  };
  const std::array<Case, 2> cases = {{{1e-10, 1e12}, {1e-16, 1e18}}};
  for (const auto& test : cases) {
    SCOPED_TRACE(test.shear);
    RendererGeode renderer = createRenderer();
    renderer.setSurfaceBudgetForTesting(/*maximumSurfaces=*/3u, /*maximumBytes=*/1u << 20);
    beginFrame(renderer);
    Transform2d deviceFromMaskLocal;
    // Even a roundoff-sized coefficient can move the far edge by 100 device pixels.
    deviceFromMaskLocal.data[1] = test.shear;
    deviceFromMaskLocal.data[4] = -test.width / 2;
    renderer.setTransform(deviceFromMaskLocal);
    renderer.pushMask(Box2d::FromXYWH(0, 0, test.width, 8), MaskType::Alpha);
    EXPECT_THAT(renderer.resourceStats().surfaceBudgetRejected, testing::IsTrue());
    EXPECT_THAT(renderer.resourceStats().surfaceCount, testing::Eq(3u));
    renderer.setTransform(Transform2d());
    renderer.setPaint(solidFill(css::RGBA(255, 255, 255, 255)));
    renderer.drawRect(Box2d::FromXYWH(0, 0, kViewportSize, kViewportSize), StrokeParams{});
    renderer.transitionMaskToContent();
    renderer.setPaint(solidFill(css::RGBA(0, 128, 0, 255)));
    renderer.drawRect(Box2d::FromXYWH(0, 0, kViewportSize, kViewportSize), StrokeParams{});
    renderer.popMask();
    renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
    renderer.drawRect(Box2d::FromXYWH(48, 0, 8, kViewportSize), StrokeParams{});
    renderer.endFrame();
    const RendererBitmap bitmap = renderer.takeSnapshot();
    ASSERT_THAT(bitmap.empty(), testing::IsFalse());
    // Inside the transformed AABB, but outside the sheared mask's visible y=50..58 band.
    EXPECT_THAT(PixelAt(bitmap, 32, 5), IsTransparent());
    EXPECT_THAT(PixelAt(bitmap, 52, 32), RgbaEq(255, 0, 0, 255));
  }
}

TEST_F(RendererGeodeTest, RotatedMaskRegionAdmissionFailsClosedAndRestoresClip) {
  RendererGeode renderer = createRenderer();
  renderer.setSurfaceBudgetForTesting(/*maximumSurfaces=*/3u, /*maximumBytes=*/1u << 20);
  beginFrame(renderer);
  renderer.setTransform(Transform2d::Rotate(0.5));
  renderer.pushMask(Box2d::FromXYWH(0.0, 0.0, 8.0, 8.0), MaskType::Alpha);
  EXPECT_THAT(renderer.resourceStats().surfaceBudgetRejected, testing::IsTrue());
  EXPECT_EQ(renderer.resourceStats().surfaceCount, 3u);
  renderer.setPaint(solidFill(css::RGBA(255, 255, 255, 255)));
  renderer.drawRect(Box2d::FromXYWH(0.0, 0.0, kViewportSize, kViewportSize), StrokeParams{});
  renderer.transitionMaskToContent();
  renderer.setTransform(Transform2d());
  renderer.setPaint(solidFill(css::RGBA(0, 128, 0, 255)));
  renderer.drawRect(Box2d::FromXYWH(0.0, 0.0, kViewportSize, kViewportSize), StrokeParams{});
  renderer.popMask();
  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  renderer.drawRect(Box2d::FromXYWH(48.0, 0.0, 8.0, kViewportSize), StrokeParams{});
  renderer.endFrame();
  const RendererBitmap bitmap = renderer.takeSnapshot();
  EXPECT_THAT(PixelAt(bitmap, 32, 32), IsTransparent());
  EXPECT_THAT(PixelAt(bitmap, 52, 32), RgbaEq(255, 0, 0, 255));
}

TEST_F(RendererGeodeTest, RotatedMaskRegionDirectPopRestoresClip) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);
  renderer.setTransform(Transform2d::Rotate(0.5));
  renderer.pushMask(Box2d::FromXYWH(0.0, 0.0, 8.0, 8.0), MaskType::Alpha);
  EXPECT_EQ(renderer.resourceStats().surfaceCount, 4u);
  renderer.popMask();
  renderer.setTransform(Transform2d());
  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  renderer.drawRect(Box2d::FromXYWH(0.0, 0.0, kViewportSize, kViewportSize), StrokeParams{});
  renderer.endFrame();
  EXPECT_EQ(renderer.lastFrameTimings().counters.unclosedMaskScopes, 0u);
  EXPECT_THAT(PixelAt(renderer.takeSnapshot(), 32, 32), RgbaEq(255, 0, 0, 255));
}

TEST_F(RendererGeodeTest, RotatedMaskRegionAbandonedFrameRetiresClipResources) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);
  renderer.setTransform(Transform2d::Rotate(0.5));
  renderer.pushMask(Box2d::FromXYWH(0.0, 0.0, 8.0, 8.0), MaskType::Alpha);
  EXPECT_EQ(renderer.resourceStats().surfaceCount, 4u);

  beginFrame(renderer);
  EXPECT_EQ(renderer.lastFrameTimings().counters.unclosedMaskScopes, 1u);
  renderer.setTransform(Transform2d::Rotate(0.5));
  renderer.pushMask(Box2d::FromXYWH(0.0, 0.0, 8.0, 8.0), MaskType::Alpha);
  renderer.endFrame();
  EXPECT_EQ(renderer.lastFrameTimings().counters.textureCreates, 0u);

  beginFrame(renderer);
  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  renderer.drawRect(Box2d::FromXYWH(0.0, 0.0, kViewportSize, kViewportSize), StrokeParams{});
  renderer.endFrame();
  EXPECT_THAT(PixelAt(renderer.takeSnapshot(), 32, 32), RgbaEq(255, 0, 0, 255));
}

class UnclosedFrameStackTest : public RendererGeodeTest,
                               public testing::WithParamInterface<const char*> {};

void PushFrameStack(RendererGeode& renderer, std::string_view kind) {
  if (kind == "Layer") {
    renderer.pushIsolatedLayer(1.0, MixBlendMode::Normal);
  } else if (kind == "Filter") {
    renderer.pushFilterLayer(components::FilterGraph{},
                             Box2d::FromXYWH(0.0, 0.0, kViewportSize, kViewportSize));
  } else {
    renderer.pushMask(std::nullopt, MaskType::Alpha);
  }
}

std::size_t OpenStackTextureCount(std::string_view kind) {
  return kind == "Mask" ? 2u : 1u;
}

void ExpectUnclosedStackCount(const geode::GeodeCounters& counters, std::string_view kind,
                              uint64_t expected = 1u) {
  EXPECT_EQ(counters.unclosedLayerScopes, kind == "Layer" ? expected : 0u);
  EXPECT_EQ(counters.unclosedFilterScopes, kind == "Filter" ? expected : 0u);
  EXPECT_EQ(counters.unclosedMaskScopes, kind == "Mask" ? expected : 0u);
}

TEST_P(UnclosedFrameStackTest, EndFrameRetiresOpenStackWithoutCompositingIt) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);
  const std::size_t pooledBefore = renderer.texturePoolStats().textureCount;

  PushFrameStack(renderer, GetParam());
  renderer.setPaint(solidFill(css::RGBA(0, 0, 255, 255)));
  renderer.drawRect(Box2d::FromXYWH(0.0, 0.0, kViewportSize, kViewportSize), StrokeParams{});
  renderer.endFrame();

  ExpectUnclosedStackCount(renderer.lastFrameTimings().counters, GetParam());
  EXPECT_EQ(renderer.texturePoolStats().textureCount,
            pooledBefore + OpenStackTextureCount(GetParam()));
  EXPECT_THAT(PixelAt(renderer.takeSnapshot(), 32, 32), IsTransparent());

  beginFrame(renderer);
  PushFrameStack(renderer, GetParam());
  renderer.endFrame();
  EXPECT_EQ(renderer.lastFrameTimings().counters.textureCreates, 0u);

  beginFrame(renderer);
  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  renderer.drawRect(Box2d::FromXYWH(0.0, 0.0, kViewportSize, kViewportSize), StrokeParams{});
  renderer.endFrame();
  EXPECT_THAT(PixelAt(renderer.takeSnapshot(), 32, 32), RgbaEq(255, 0, 0, 255));
}

TEST_P(UnclosedFrameStackTest, BeginFrameRetiresStackFromAbandonedFrame) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);
  PushFrameStack(renderer, GetParam());
  renderer.setPaint(solidFill(css::RGBA(0, 0, 255, 255)));
  renderer.drawRect(Box2d::FromXYWH(0.0, 0.0, kViewportSize, kViewportSize), StrokeParams{});

  beginFrame(renderer);
  ExpectUnclosedStackCount(renderer.lastFrameTimings().counters, GetParam());
  PushFrameStack(renderer, GetParam());
  renderer.endFrame();
  ExpectUnclosedStackCount(renderer.lastFrameTimings().counters, GetParam(), 2u);
  EXPECT_EQ(renderer.lastFrameTimings().counters.textureCreates, 0u);
  EXPECT_THAT(PixelAt(renderer.takeSnapshot(), 32, 32), IsTransparent());

  beginFrame(renderer);
  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  renderer.drawRect(Box2d::FromXYWH(0.0, 0.0, kViewportSize, kViewportSize), StrokeParams{});
  renderer.endFrame();
  EXPECT_THAT(PixelAt(renderer.takeSnapshot(), 32, 32), RgbaEq(255, 0, 0, 255));
}

INSTANTIATE_TEST_SUITE_P(OpenLayerFilterAndMask, UnclosedFrameStackTest,
                         testing::Values("Layer", "Filter", "Mask"),
                         [](const testing::TestParamInfo<const char*>& info) {
                           return std::string(info.param);
                         });

TEST_F(RendererGeodeTest, AnUnclosedFilterDropsActiveAndSavedClipsAtFrameEnd) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);
  ResolvedClip outerClip;
  outerClip.clipRect = Box2d::FromXYWH(0.0, 0.0, 8.0, 8.0);
  renderer.pushClip(outerClip);
  renderer.pushFilterLayer(components::FilterGraph{},
                           Box2d::FromXYWH(0.0, 0.0, kViewportSize, kViewportSize));
  ResolvedClip innerClip;
  innerClip.clipRect = Box2d::FromXYWH(0.0, 0.0, 8.0, 8.0);
  renderer.pushClip(innerClip);
  renderer.endFrame();

  beginFrame(renderer);
  renderer.popFilterLayer();
  ResolvedClip nextFrameClip;
  nextFrameClip.clipRect = Box2d::FromXYWH(0.0, 0.0, kViewportSize, kViewportSize);
  renderer.pushClip(nextFrameClip);
  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  renderer.drawRect(Box2d::FromXYWH(0.0, 0.0, kViewportSize, kViewportSize), StrokeParams{});
  renderer.popClip();
  renderer.endFrame();
  EXPECT_THAT(PixelAt(renderer.takeSnapshot(), 32, 32), RgbaEq(255, 0, 0, 255));
}

TEST_F(RendererGeodeTest, NestedUnclosedScopesReturnAllOffscreensWithoutCompositing) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);
  const std::size_t pooledBefore = renderer.texturePoolStats().textureCount;
  renderer.pushIsolatedLayer(1.0, MixBlendMode::Normal);
  renderer.pushFilterLayer(components::FilterGraph{},
                           Box2d::FromXYWH(0.0, 0.0, kViewportSize, kViewportSize));
  renderer.pushMask(std::nullopt, MaskType::Alpha);
  renderer.setPaint(solidFill(css::RGBA(0, 0, 255, 255)));
  renderer.drawRect(Box2d::FromXYWH(0.0, 0.0, kViewportSize, kViewportSize), StrokeParams{});
  renderer.endFrame();

  const geode::GeodeCounters counters = renderer.lastFrameTimings().counters;
  EXPECT_EQ(counters.unclosedLayerScopes, 1u);
  EXPECT_EQ(counters.unclosedFilterScopes, 1u);
  EXPECT_EQ(counters.unclosedMaskScopes, 1u);
  EXPECT_EQ(renderer.texturePoolStats().textureCount, pooledBefore + 4u);
  EXPECT_THAT(PixelAt(renderer.takeSnapshot(), 32, 32), IsTransparent());

  beginFrame(renderer);
  renderer.pushIsolatedLayer(1.0, MixBlendMode::Normal);
  renderer.pushFilterLayer(components::FilterGraph{},
                           Box2d::FromXYWH(0.0, 0.0, kViewportSize, kViewportSize));
  renderer.pushMask(std::nullopt, MaskType::Alpha);
  renderer.endFrame();
  EXPECT_EQ(renderer.lastFrameTimings().counters.textureCreates, 0u);
}

TEST_F(RendererGeodeTest, AnUnmatchedClipAtFrameEndDoesNotClipTheNextFrame) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  ResolvedClip clip;
  clip.clipRect = Box2d::FromXYWH(0.0, 0.0, 8.0, 8.0);
  renderer.pushClip(clip);
  renderer.endFrame();

  beginFrame(renderer);
  ResolvedClip nextFrameClip;
  nextFrameClip.clipRect = Box2d::FromXYWH(0.0, 0.0, kViewportSize, kViewportSize);
  renderer.pushClip(nextFrameClip);
  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  renderer.drawRect(Box2d::FromXYWH(0.0, 0.0, kViewportSize, kViewportSize), StrokeParams{});
  renderer.popClip();
  renderer.endFrame();

  EXPECT_THAT(PixelAt(renderer.takeSnapshot(), 32, 32), RgbaEq(255, 0, 0, 255));
}

TEST_F(RendererGeodeTest, AnUnmatchedClipInAnAbandonedFrameDoesNotClipTheNextFrame) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  ResolvedClip clip;
  clip.clipRect = Box2d::FromXYWH(0.0, 0.0, 8.0, 8.0);
  renderer.pushClip(clip);

  beginFrame(renderer);
  ResolvedClip nextFrameClip;
  nextFrameClip.clipRect = Box2d::FromXYWH(0.0, 0.0, kViewportSize, kViewportSize);
  renderer.pushClip(nextFrameClip);
  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  renderer.drawRect(Box2d::FromXYWH(0.0, 0.0, kViewportSize, kViewportSize), StrokeParams{});
  renderer.popClip();
  renderer.endFrame();

  EXPECT_THAT(PixelAt(renderer.takeSnapshot(), 32, 32), RgbaEq(255, 0, 0, 255));
}

TEST_F(RendererGeodeTest, AnOpenPatternRestoresNoUnmatchedOuterClipIntoTheNextFrame) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  ResolvedClip outerClip;
  outerClip.clipRect = Box2d::FromXYWH(0.0, 0.0, 8.0, 8.0);
  renderer.pushClip(outerClip);
  ASSERT_TRUE(renderer.beginPatternTile(Box2d::FromXYWH(0.0, 0.0, 8.0, 8.0), Transform2d()));
  renderer.endFrame();

  beginFrame(renderer);
  ResolvedClip nextFrameClip;
  nextFrameClip.clipRect = Box2d::FromXYWH(0.0, 0.0, kViewportSize, kViewportSize);
  renderer.pushClip(nextFrameClip);
  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  renderer.drawRect(Box2d::FromXYWH(0.0, 0.0, kViewportSize, kViewportSize), StrokeParams{});
  renderer.popClip();
  renderer.endFrame();

  EXPECT_THAT(PixelAt(renderer.takeSnapshot(), 32, 32), RgbaEq(255, 0, 0, 255));
}

TEST_F(RendererGeodeTest, ARecycledPatternTileNeverShowsWhatTheLastFramePaintedIntoIt) {
  ASSERT_TRUE(sharedDevice() != nullptr);
  RendererGeode renderer = createRenderer();

  // Paint the tile opaque, and consume it, so it goes back to the pool holding those texels.
  beginFrame(renderer);
  ASSERT_TRUE(renderer.beginPatternTile(Box2d::FromXYWH(0.0, 0.0, 8.0, 8.0), Transform2d()));
  renderer.setPaint(solidFill(css::RGBA(0, 0, 255, 255)));
  renderer.drawRect(Box2d::FromXYWH(0.0, 0.0, 8.0, 8.0), StrokeParams{});
  renderer.endPatternTile(/*forStroke=*/false, std::nullopt);
  renderer.drawRect(Box2d::FromXYWH(0.0, 0.0, kViewportSize, kViewportSize), StrokeParams{});
  renderer.endFrame();

  // Same extent, so the pool hands the same texture back, and this tile is drawn into by nothing.
  beginFrame(renderer);
  ASSERT_TRUE(renderer.beginPatternTile(Box2d::FromXYWH(0.0, 0.0, 8.0, 8.0), Transform2d()));
  renderer.endPatternTile(/*forStroke=*/false, std::nullopt);
  renderer.setPaint(solidFill(css::RGBA(0, 0, 255, 255)));
  renderer.drawRect(Box2d::FromXYWH(0.0, 0.0, kViewportSize, kViewportSize), StrokeParams{});
  renderer.endFrame();

  ASSERT_EQ(renderer.lastFrameTimings().counters.textureCreates, 0u)
      << "The second tile must be the recycled one, otherwise this proves nothing about reuse";

  const RendererBitmap bitmap = renderer.takeSnapshot();
  EXPECT_THAT(PixelAt(bitmap, 32, 32), IsTransparent())
      << "A tile out of the pool carries whatever was last rendered into it, which may belong to "
         "another document sharing the device, so an undrawn tile must sample as cleared";
}

/// Parses \p source, which must parse, into a document.
/// @param source SVG document text.
SVGDocument ParsePatternDocument(std::string_view source) {
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  ParseResult<SVGDocument> parsed = parser::SVGParser::ParseSVG(source, warnings);
  EXPECT_THAT(parsed, NoParseError());
  return std::move(parsed.result());
}

/// A rect filled with a pattern whose tile holds a solid cell, a gradient-painted cell, and a cell
/// painted with a nested pattern; two lines stroked with different patterns whose shared marker
/// paints a square with each line's stroke as its context paint, so one marker content draws with
/// two pattern tiles in a frame; and three rects filled with three different patterns whose tiles
/// have one size, so they share a pool bucket.
constexpr std::string_view kPatternEditSvg = R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="96" height="80" viewBox="0 0 96 80">
  <defs>
    <linearGradient id="g">
      <stop offset="0" stop-color="#2050c0"/>
      <stop id="g-end" offset="1" stop-color="#20c050"/>
    </linearGradient>
    <pattern id="inner" patternUnits="userSpaceOnUse" width="4" height="4">
      <rect id="inner-cell" width="2" height="2" fill="#c02050"/>
    </pattern>
    <pattern id="p" patternUnits="userSpaceOnUse" width="16" height="16">
      <rect id="cell" width="8" height="8" fill="#c05020"/>
      <rect x="8" width="8" height="8" fill="url(#g)"/>
      <rect y="8" width="16" height="8" fill="url(#inner)"/>
    </pattern>
    <pattern id="q" patternUnits="userSpaceOnUse" width="6" height="6">
      <rect width="3" height="6" fill="#806020"/>
    </pattern>
    <pattern id="r" patternUnits="userSpaceOnUse" width="8" height="8">
      <rect width="4" height="8" fill="#a02080"/>
    </pattern>
    <pattern id="s" patternUnits="userSpaceOnUse" width="8" height="8">
      <rect width="8" height="4" fill="#20a080"/>
    </pattern>
    <pattern id="t" patternUnits="userSpaceOnUse" width="8" height="8">
      <rect width="4" height="4" fill="#2080a0"/>
    </pattern>
    <marker id="m" markerUnits="userSpaceOnUse" markerWidth="20" markerHeight="20"
            overflow="visible">
      <rect width="20" height="20" fill="context-stroke"/>
    </marker>
  </defs>
  <rect x="4" y="4" width="60" height="56" fill="url(#p)"/>
  <path d="M 70 4 L 90 30" stroke="url(#p)" stroke-width="2" marker-start="url(#m)"/>
  <path d="M 70 40 L 90 60" stroke="url(#q)" stroke-width="2" marker-start="url(#m)"/>
  <rect x="4" y="64" width="24" height="12" fill="url(#r)"/>
  <rect x="36" y="64" width="24" height="12" fill="url(#s)"/>
  <rect x="68" y="64" width="24" height="12" fill="url(#t)"/>
</svg>)svg";

/// An edit applied to a document drawn with \ref kPatternEditSvg.
struct PatternEdit {
  /// How the edit reaches the document.
  enum class Kind {
    Attribute,     //!< `setAttribute` on \ref selector.
    PatternWidth,  //!< `SVGPatternElement::setWidth` on \ref selector, to \ref value user units.
    Zoom,          //!< The canvas doubles in size, so the viewBox maps to twice the pixels.
  };
  const char* name;       //!< Names the case and its diff images.
  Kind kind;              //!< How the edit is applied.
  const char* selector;   //!< Element an attribute or pattern width edit changes.
  const char* attribute;  //!< Attribute set by an attribute edit.
  const char* value;      //!< New attribute value, or the new pattern width.
};

/// Applies \p edit to \p document.
void ApplyPatternEdit(SVGDocument& document, const PatternEdit& edit) {
  switch (edit.kind) {
    case PatternEdit::Kind::Zoom: document.setCanvasSize(192, 128); return;
    case PatternEdit::Kind::PatternWidth: {
      std::optional<SVGElement> pattern = document.querySelector(edit.selector);
      ASSERT_TRUE(pattern.has_value()) << edit.selector;
      pattern->cast<SVGPatternElement>().setWidth(Lengthd(std::stod(edit.value)));
      return;
    }
    case PatternEdit::Kind::Attribute: {
      std::optional<SVGElement> element = document.querySelector(edit.selector);
      ASSERT_TRUE(element.has_value()) << edit.selector;
      element->setAttribute(edit.attribute, edit.value);
      return;
    }
  }
}

/// A renderer that drew a document's patterns on earlier frames draws an edit of anything a tile's
/// pixels depend on exactly as a renderer that never saw the document before: a shape inside the
/// pattern, the pattern's size and transform, a gradient or a nested pattern its content paints
/// with, the device scale, and an edit that makes tiles of one size trade textures, so a resident
/// fill must rebind. Every case also draws one marker's content with two context paints in a
/// frame. Context paint edits are not covered: a warm renderer keeps a marker's old context paint
/// (#2617), and `setAttribute` of a pattern's `width` does not reach the tile (#2616), so the size
/// edit goes through `SVGPatternElement::setWidth`.
TEST_F(RendererGeodeTest, PatternEditsReachTheNextFrameOfAWarmRenderer) {
  ASSERT_TRUE(sharedDevice() != nullptr);
  using Kind = PatternEdit::Kind;
  constexpr PatternEdit kEdits[] = {
      {"shape_paint", Kind::Attribute, "#cell", "fill", "#20a0c0"},
      {"shape_geometry", Kind::Attribute, "#cell", "width", "4"},
      {"pattern_size", Kind::PatternWidth, "#p", nullptr, "24"},
      {"pattern_transform", Kind::Attribute, "#p", "patternTransform", "rotate(30)"},
      {"gradient_stop", Kind::Attribute, "#g-end", "stop-color", "#f0f020"},
      {"nested_pattern", Kind::Attribute, "#inner-cell", "fill", "#f0a000"},
      {"zoom", Kind::Zoom, nullptr, nullptr, nullptr},
      // The first rect's tile changes size, so the second and third rects' tiles, still of the
      // old size, take the textures the first and second of that size used.
      {"tile_textures_trade", Kind::PatternWidth, "#r", nullptr, "12"},
  };
  for (const PatternEdit& edit : kEdits) {
    SCOPED_TRACE(edit.name);
    SVGDocument document = ParsePatternDocument(kPatternEditSvg);
    SVGDocument reference = ParsePatternDocument(kPatternEditSvg);

    RendererGeode warm = createRenderer();
    warm.draw(document);
    warm.draw(document);
    const RendererBitmap beforeEdit = warm.takeSnapshot();
    ASSERT_THAT(test::CountNonTransparentPixels(beforeEdit), testing::Gt(0u))
        << "Premise: the patterns must paint";

    ASSERT_NO_FATAL_FAILURE(ApplyPatternEdit(document, edit));
    ASSERT_NO_FATAL_FAILURE(ApplyPatternEdit(reference, edit));
    warm.draw(document);
    const RendererBitmap afterEdit = warm.takeSnapshot();

    RendererGeode fresh = createRenderer();
    fresh.draw(reference);
    const RendererBitmap expected = fresh.takeSnapshot();
    // Premise: the edit changes the rendering, or a stale tile would pass unnoticed. An edit that
    // changes the canvas size changes it by construction. The comparison only measures the
    // difference, so its pixel budget is unbounded.
    if (beforeEdit.dimensions == expected.dimensions) {
      editor::tests::BitmapGoldenCompareParams signalParams =
          editor::tests::PixelmatchIdentityParams();
      signalParams.maxMismatchedPixels = std::numeric_limits<int>::max();
      int editedPixels = -1;
      editor::tests::CompareBitmapToBitmap(beforeEdit, expected,
                                           std::string("pattern_edit_premise_") + edit.name,
                                           signalParams, &editedPixels);
      ASSERT_THAT(editedPixels, testing::Gt(0))
          << "Premise: the edit must change the rendering, or a stale tile would pass unnoticed";
    }

    editor::tests::CompareBitmapToBitmap(afterEdit, expected,
                                         std::string("pattern_edit_") + edit.name,
                                         editor::tests::PixelmatchIdentityParams());
  }
}

/// A resident pattern fill paints exactly what the per-frame arena pattern fill paints, which a
/// renderer whose document budget keeps no resident bytes draws. The triangle is rotated, and the
/// pattern too, so a wrong mapping from path to tile cannot match. The same triangle in a nested
/// viewport, whose clip rectangle the rotation turns into a clip polygon, is resident as well and
/// paints the same: the viewport reaches far past the triangle on every side, so the clip itself
/// changes no pixel.
TEST_F(RendererGeodeTest, ResidentPatternFillMatchesTheArenaPatternFill) {
  ASSERT_THAT(sharedDevice(), testing::NotNull());
  constexpr std::string_view kDefs = R"svg(
    <pattern id="p" patternUnits="userSpaceOnUse" width="10" height="10"
             patternTransform="rotate(20)">
      <rect width="5" height="5" fill="#2050c0"/>
      <rect x="5" y="5" width="5" height="5" fill="#c05020"/>
    </pattern>)svg";
  const auto triangle = [](std::string_view fill) {
    return R"svg(<path d="M 8 4 L 88 12 L 60 60 Z" fill-opacity="0.6" fill=")svg" +
           std::string(fill) + "\"/>";
  };
  // The nested viewport's user space is the outer one: its origin sits at (-100, -100), and its
  // content is moved back by (100, 100).
  const auto inViewport = [](const std::string& content) {
    return R"svg(<svg x="-100" y="-100" width="300" height="300" overflow="hidden">)svg"
           R"svg(<g transform="translate(100 100)">)svg" +
           content + "</g></svg>";
  };
  const auto rotated = [](const std::string& content) {
    return R"svg(<g transform="rotate(30 48 32)">)svg" + content + "</g>";
  };
  const std::string head =
      std::string(
          R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="96" height="64"><defs>)svg") +
      std::string(kDefs) + "</defs>";
  const std::string source = head + rotated(triangle("url(#p)")) + "</svg>";
  SVGDocument resident = ParsePatternDocument(source);
  SVGDocument arena = ParsePatternDocument(source);
  SVGDocument underPolygon =
      ParsePatternDocument(head + rotated(inViewport(triangle("url(#p)"))) + "</svg>");

  const auto steadyFrame = [&](SVGDocument& document, std::uint64_t residentBytes) {
    RendererGeode renderer = createRenderer();
    renderer.setGeometryBudgetForTesting(/*maximumDraws=*/4096u, /*maximumItems=*/1u << 20,
                                         /*maximumFrameBytes=*/64u << 20,
                                         /*maximumCacheBytes=*/64u << 20,
                                         /*maximumResidentBytes=*/residentBytes);
    renderer.draw(document);
    renderer.draw(document);
    const geode::GeodeCounters counters = renderer.lastFrameTimings().counters;
    return std::make_tuple(counters.bufferWrites, counters.bindgroupCreates,
                           renderer.takeSnapshot());
  };
  const auto [residentWrites, residentBindGroups, residentBitmap] =
      steadyFrame(resident, 64u << 20);
  const auto [arenaWrites, arenaBindGroups, arenaBitmap] = steadyFrame(arena, 0u);
  const auto [polygonWrites, polygonBindGroups, polygonBitmap] =
      steadyFrame(underPolygon, 64u << 20);
  ASSERT_THAT(residentWrites, testing::Eq(0u)) << "Premise: the unclipped fill is resident";
  ASSERT_THAT(residentBindGroups, testing::Eq(0u)) << "Premise: the unclipped fill is resident";
  ASSERT_THAT(arenaBindGroups, testing::Gt(0u))
      << "Premise: with no resident bytes the fill builds a bind group of its own every frame, so "
         "it draws from the arena";
  ASSERT_THAT(test::CountNonTransparentPixels(residentBitmap), testing::Gt(0u));
  EXPECT_THAT(polygonWrites, testing::Eq(0u)) << "the fill under a clip polygon is resident";
  EXPECT_THAT(polygonBindGroups, testing::Eq(0u)) << "the fill under a clip polygon is resident";

  editor::tests::CompareBitmapToBitmap(residentBitmap, arenaBitmap, "resident_vs_arena_pattern",
                                       editor::tests::PixelmatchIdentityParams());
  editor::tests::CompareBitmapToBitmap(polygonBitmap, residentBitmap,
                                       "resident_under_polygon_pattern",
                                       editor::tests::PixelmatchIdentityParams());
}

/// A renderer keeps a view of each pattern tile texture only while its frames keep drawing into
/// that texture, so however many distinct tiles a run of frames draws, the views it holds are
/// those of its last two frames. The tile textures themselves go back to the device's pool at
/// every frame end, whose byte budget bounds them
/// (`TransientTexturePoolStaysWithinGlobalMemoryBudgetAcrossSizeChurn`).
TEST_F(RendererGeodeTest, PatternTileViewsStayBoundedBetweenFrames) {
  ASSERT_TRUE(sharedDevice() != nullptr);
  RendererGeode renderer = createRenderer();
  constexpr int kTilesPerFrame = 16;
  constexpr int kFrames = 8;
  const auto drawFrame = [&](int firstWidth, int tileCount) {
    beginFrame(renderer);
    for (int index = 0; index < tileCount; ++index) {
      const double width = static_cast<double>(firstWidth + index);
      ASSERT_TRUE(renderer.beginPatternTile(Box2d::FromXYWH(0.0, 0.0, width, 8.0), Transform2d()))
          << "Premise: every tile is admitted";
      renderer.setPaint(solidFill(css::RGBA(0, 0, 255, 255)));
      renderer.drawRect(Box2d::FromXYWH(0.0, 0.0, 4.0, 4.0), StrokeParams{});
      renderer.endPatternTile(/*forStroke=*/false, std::nullopt);
      renderer.drawRect(Box2d::FromXYWH(0.0, 0.0, kViewportSize, kViewportSize), StrokeParams{});
    }
    renderer.endFrame();
  };

  // Every tile of every frame has a size no other tile has, so no frame draws into a texture an
  // earlier frame drew into.
  for (int frame = 0; frame < kFrames; ++frame) {
    ASSERT_NO_FATAL_FAILURE(drawFrame(8 + frame * kTilesPerFrame, kTilesPerFrame));
    EXPECT_THAT(renderer.patternTileViewCountForTesting(), testing::Le(2u * kTilesPerFrame))
        << "Views are kept for the current and the previous frame's tiles only";
  }

  ASSERT_NO_FATAL_FAILURE(drawFrame(0, 0));
  ASSERT_NO_FATAL_FAILURE(drawFrame(0, 0));
  EXPECT_THAT(renderer.patternTileViewCountForTesting(), testing::Eq(0u))
      << "Two frames without a pattern leave no tile view behind";
}

/// Three clipped columns: a gradient-filled group under a clip with a curved path, an
/// objectBoundingBox clip over a rect, a clip whose own clip-path nests a second clip, and a clip
/// with a hole whose clip-rule decides whether the hole is clipped out.
constexpr std::string_view kClipEditSvg = R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="144" height="64" viewBox="0 0 144 64">
  <defs>
    <clipPath id="c">
      <path id="c-shape" d="M 6 32 C 6 6 42 6 42 32 C 42 58 6 58 6 32 Z"/>
    </clipPath>
    <clipPath id="obb" clipPathUnits="objectBoundingBox">
      <path id="obb-shape" d="M 0.1 0.1 L 0.9 0.2 L 0.5 0.9 Z"/>
    </clipPath>
    <clipPath id="inner">
      <rect id="inner-shape" x="52" y="34" width="20" height="28"/>
    </clipPath>
    <clipPath id="nested" clip-path="url(#inner)">
      <circle cx="68" cy="48" r="14"/>
    </clipPath>
    <clipPath id="rule">
      <path id="rule-shape" d="M 100 4 H 140 V 60 H 100 Z M 108 12 H 132 V 52 H 108 Z"/>
    </clipPath>
    <linearGradient id="g" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0" stop-color="#2050c0"/>
      <stop offset="1" stop-color="#c05020"/>
    </linearGradient>
  </defs>
  <g id="clipped" clip-path="url(#c)">
    <rect width="48" height="64" fill="url(#g)"/>
  </g>
  <rect id="obb-target" x="50" y="2" width="44" height="28" fill="#c05020"
        clip-path="url(#obb)"/>
  <rect id="nested-target" x="48" y="32" width="48" height="32" fill="#2050c0"
        clip-path="url(#nested)"/>
  <rect id="rule-target" x="96" width="48" height="64" fill="#208040" clip-path="url(#rule)"/>
</svg>)svg";

/// An edit applied to a document drawn with \ref kClipEditSvg.
struct ClipEdit {
  /// How the edit reaches the document.
  enum class Kind {
    Attribute,       //!< `setAttribute` on \ref selector.
    UserSpaceUnits,  //!< `SVGClipPathElement::setClipPathUnits(UserSpaceOnUse)` on \ref selector.
    Zoom,            //!< The canvas doubles in size, so the viewBox maps to twice the pixels.
    ZoomFar,         //!< The canvas grows eightfold, far enough to need finer clip curves.
  };
  const char* name;       //!< Names the case and its diff images.
  Kind kind;              //!< How the edit is applied.
  const char* selector;   //!< Element the edit changes.
  const char* attribute;  //!< Attribute set by an attribute edit.
  const char* value;      //!< New attribute value.
};

/// Applies \p edit to \p document.
void ApplyClipEdit(SVGDocument& document, const ClipEdit& edit) {
  if (edit.kind == ClipEdit::Kind::Zoom || edit.kind == ClipEdit::Kind::ZoomFar) {
    const int scale = edit.kind == ClipEdit::Kind::Zoom ? 2 : 8;
    document.setCanvasSize(144 * scale, 64 * scale);
    return;
  }
  std::optional<SVGElement> element = document.querySelector(edit.selector);
  ASSERT_THAT(element, testing::Optional(testing::_)) << edit.selector;
  if (edit.kind == ClipEdit::Kind::UserSpaceUnits) {
    element->cast<SVGClipPathElement>().setClipPathUnits(ClipPathUnits::UserSpaceOnUse);
    return;
  }
  element->setAttribute(edit.attribute, edit.value);
}

/// A renderer that drew a document's clips on earlier frames draws an edit of anything a clip mask
/// depends on exactly as a renderer that never saw the document: the clip shape's geometry and
/// transform, its clip-rule on the shape and on the clipped element, clipPathUnits, the clipped
/// element's bounds under an objectBoundingBox clip and its transform, the nested clip, the clip
/// reference itself, and the device scale. The warm renderer keeps clip encodes, resident mask
/// fills, and clipped draws bound to retained mask views, so each edit must reach all of them.
TEST_F(RendererGeodeTest, ClipEditsReachTheNextFrameOfAWarmRenderer) {
  ASSERT_THAT(sharedDevice(), testing::NotNull());
  using Kind = ClipEdit::Kind;
  constexpr ClipEdit kEdits[] = {
      {"clip_geometry", Kind::Attribute, "#c-shape", "d",
       "M 4 20 C 20 0 44 10 40 40 C 30 62 4 56 4 20 Z"},
      {"clip_shape_transform", Kind::Attribute, "#c-shape", "transform", "translate(3 2)"},
      {"clip_rule_on_shape", Kind::Attribute, "#rule-shape", "clip-rule", "evenodd"},
      {"clip_rule_on_clipped", Kind::Attribute, "#rule-target", "clip-rule", "evenodd"},
      // `setAttribute` of `clipPathUnits` does not reach the rendering of a parsed document, so
      // the units change through the element's own setter.
      {"clip_path_units", Kind::UserSpaceUnits, "#obb", nullptr, nullptr},
      {"clipped_bounds", Kind::Attribute, "#obb-target", "width", "30"},
      {"clipped_transform", Kind::Attribute, "#clipped", "transform", "translate(3 1)"},
      {"nested_clip", Kind::Attribute, "#inner-shape", "width", "10"},
      {"clip_reference", Kind::Attribute, "#obb-target", "clip-path", "url(#c)"},
      {"zoom", Kind::Zoom, nullptr, nullptr, nullptr},
      {"zoom_far", Kind::ZoomFar, nullptr, nullptr, nullptr},
  };
  for (const ClipEdit& edit : kEdits) {
    SCOPED_TRACE(edit.name);
    SVGDocument document = ParsePatternDocument(kClipEditSvg);
    SVGDocument reference = ParsePatternDocument(kClipEditSvg);

    RendererGeode warm = createRenderer();
    warm.draw(document);
    warm.draw(document);
    const RendererBitmap beforeEdit = warm.takeSnapshot();
    ASSERT_THAT(test::CountNonTransparentPixels(beforeEdit), testing::Gt(0u))
        << "Premise: the clipped shapes must paint";

    ASSERT_NO_FATAL_FAILURE(ApplyClipEdit(document, edit));
    ASSERT_NO_FATAL_FAILURE(ApplyClipEdit(reference, edit));
    warm.draw(document);
    const RendererBitmap afterEdit = warm.takeSnapshot();

    RendererGeode fresh = createRenderer();
    fresh.draw(reference);
    const RendererBitmap expected = fresh.takeSnapshot();
    // Premise: the edit changes the rendering, or a stale clip would pass unnoticed. A zoom
    // changes the canvas size by construction. The comparison only measures the difference, so
    // its pixel budget is unbounded.
    if (beforeEdit.dimensions == expected.dimensions) {
      editor::tests::BitmapGoldenCompareParams signalParams =
          editor::tests::PixelmatchIdentityParams();
      signalParams.maxMismatchedPixels = std::numeric_limits<int>::max();
      int editedPixels = -1;
      editor::tests::CompareBitmapToBitmap(beforeEdit, expected,
                                           std::string("clip_edit_premise_") + edit.name,
                                           signalParams, &editedPixels);
      ASSERT_THAT(editedPixels, testing::Gt(0))
          << "Premise: the edit must change the rendering, or a stale clip would pass unnoticed";
    }

    editor::tests::CompareBitmapToBitmap(afterEdit, expected, std::string("clip_edit_") + edit.name,
                                         editor::tests::PixelmatchIdentityParams());
  }
}

/// The documents of \ref ResidentClippedDrawsMatchTheArenaDraws: a solid fill, a gradient fill
/// with a gradient stroke, and a pattern fill under a clip with a curved path, all rotated. With
/// \p polygonClip the group sits in a nested viewport, whose clip rectangle the rotation turns
/// into a clip polygon, which each draw's uniform carries together with the clip mask. The
/// viewport reaches far past the content on every side, so it changes no pixel itself.
std::string ClippedDrawsDocument(bool polygonClip) {
  const std::string content = R"svg(
    <g clip-path="url(#round)">
      <rect x="0" y="0" width="96" height="24" fill="#e0c020"/>
      <polygon points="8,26 88,26 48,46" fill="url(#g)" stroke="url(#g)" stroke-width="3"/>
      <rect x="0" y="44" width="96" height="20" fill="url(#p)"/>
    </g>)svg";
  const std::string body =
      polygonClip ? R"svg(<svg x="-100" y="-100" width="300" height="300" overflow="hidden">)svg"
                    R"svg(<g transform="translate(100 100)">)svg" +
                        content + "</g></svg>"
                  : content;
  return R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="96" height="64">
    <defs>
      <clipPath id="round">
        <path d="M 8 32 C 8 4 88 4 88 32 C 88 60 8 60 8 32 Z"/>
      </clipPath>
      <linearGradient id="g" x1="0" y1="0" x2="1" y2="1">
        <stop offset="0" stop-color="#2050c0"/>
        <stop offset="1" stop-color="#c05020"/>
      </linearGradient>
      <pattern id="p" patternUnits="userSpaceOnUse" width="10" height="10"
               patternTransform="rotate(20)">
        <rect width="5" height="5" fill="#208040"/>
      </pattern>
    </defs>
    <g transform="rotate(12 48 32)">)svg" +
         body + "</g></svg>";
}

/// Draws under a path clip mask paint exactly what the per-frame arena paints for the same draws,
/// which a renderer whose document budget keeps no resident bytes draws: a resident solid,
/// gradient, gradient stroke and pattern fill bind the clip mask's retained view and carry the
/// same clip flag in their uniforms as arena draws do. Under a clip polygon as well, they stay
/// resident and paint the same.
TEST_F(RendererGeodeTest, ResidentClippedDrawsMatchTheArenaDraws) {
  ASSERT_THAT(sharedDevice(), testing::NotNull());
  SVGDocument resident = ParsePatternDocument(ClippedDrawsDocument(/*polygonClip=*/false));
  SVGDocument arena = ParsePatternDocument(ClippedDrawsDocument(/*polygonClip=*/false));
  SVGDocument underPolygon = ParsePatternDocument(ClippedDrawsDocument(/*polygonClip=*/true));

  const auto steadyFrame = [&](SVGDocument& document, std::uint64_t residentBytes) {
    RendererGeode renderer = createRenderer();
    renderer.setGeometryBudgetForTesting(/*maximumDraws=*/4096u, /*maximumItems=*/1u << 20,
                                         /*maximumFrameBytes=*/64u << 20,
                                         /*maximumCacheBytes=*/64u << 20,
                                         /*maximumResidentBytes=*/residentBytes);
    renderer.draw(document);
    renderer.draw(document);
    const geode::GeodeCounters counters = renderer.lastFrameTimings().counters;
    return std::make_tuple(counters.bindgroupCreates, renderer.takeSnapshot());
  };
  const auto [residentBindGroups, residentBitmap] = steadyFrame(resident, 64u << 20);
  const auto [arenaBindGroups, arenaBitmap] = steadyFrame(arena, 0u);
  const auto [polygonBindGroups, polygonBitmap] = steadyFrame(underPolygon, 64u << 20);
  ASSERT_THAT(residentBindGroups, testing::Eq(0u))
      << "Premise: the clipped draws and the clip's mask fill are resident";
  ASSERT_THAT(arenaBindGroups, testing::Ge(4u))
      << "Premise: with no resident bytes every clipped draw builds a bind group of its own";
  ASSERT_THAT(test::CountNonTransparentPixels(residentBitmap), testing::Gt(0u));
  EXPECT_THAT(polygonBindGroups, testing::Eq(0u))
      << "the clipped draws under a clip polygon and the clip's mask fill are resident";

  editor::tests::CompareBitmapToBitmap(residentBitmap, arenaBitmap, "resident_vs_arena_clipped",
                                       editor::tests::PixelmatchIdentityParams());
  editor::tests::CompareBitmapToBitmap(polygonBitmap, residentBitmap,
                                       "resident_under_polygon_clipped",
                                       editor::tests::PixelmatchIdentityParams());
}

/// Clip geometry is charged to the document's geometry budget. A document whose budget refuses
/// to keep clip encodes or resident clip geometry still renders exactly what an unbounded renderer
/// renders: past the cache budget each frame encodes its clip shapes again, and past the resident
/// budget each frame fills them from the per-frame arena.
TEST_F(RendererGeodeTest, ClipGeometryPastTheDocumentBudgetStillRenders) {
  ASSERT_THAT(sharedDevice(), testing::NotNull());
  RendererGeode unbounded = createRenderer();
  SVGDocument unboundedDocument = ParsePatternDocument(kClipEditSvg);
  unbounded.draw(unboundedDocument);
  unbounded.draw(unboundedDocument);
  const RendererBitmap expected = unbounded.takeSnapshot();
  const geode::GeodeCounters unboundedCounters = unbounded.lastFrameTimings().counters;
  ASSERT_THAT(unboundedCounters.pathEncodes, testing::Eq(0u))
      << "Premise: within the budget the clip shapes keep their encodes";
  ASSERT_THAT(unboundedCounters.bufferWrites, testing::Eq(0u))
      << "Premise: within the budget the clip shapes and clipped draws are resident";

  // The clip shapes of kClipEditSvg: the curved clip, the objectBoundingBox triangle, the nested
  // clip's rectangle and circle and its group's union, and the clip with a hole.
  constexpr uint64_t kClipShapes = 6u;
  // The content of kClipEditSvg without its clips, whose encodes and bind groups count the content
  // alone.
  constexpr std::string_view kUnclippedSvg = R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="144" height="64" viewBox="0 0 144 64">
  <defs>
    <linearGradient id="g" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0" stop-color="#2050c0"/>
      <stop offset="1" stop-color="#c05020"/>
    </linearGradient>
  </defs>
  <g id="clipped">
    <rect width="48" height="64" fill="url(#g)"/>
  </g>
  <rect id="obb-target" x="50" y="2" width="44" height="28" fill="#c05020"/>
  <rect id="nested-target" x="48" y="32" width="48" height="32" fill="#2050c0"/>
  <rect id="rule-target" x="96" width="48" height="64" fill="#208040"/>
</svg>)svg";

  struct Bound {
    const char* name;
    std::uint64_t cacheBytes;
    std::uint64_t residentBytes;
  };
  constexpr Bound kBounds[] = {{"no_cache_bytes", 0u, 64u << 20},
                               {"no_resident_bytes", 64u << 20, 0u}};
  for (const Bound& bound : kBounds) {
    SCOPED_TRACE(bound.name);
    const auto steadyCounters = [&](SVGDocument& document) {
      RendererGeode renderer = createRenderer();
      renderer.setGeometryBudgetForTesting(/*maximumDraws=*/4096u, /*maximumItems=*/1u << 20,
                                           /*maximumFrameBytes=*/64u << 20,
                                           /*maximumCacheBytes=*/bound.cacheBytes,
                                           /*maximumResidentBytes=*/bound.residentBytes);
      renderer.draw(document);
      renderer.draw(document);
      return std::make_tuple(renderer.lastFrameTimings().counters, renderer.takeSnapshot());
    };
    SVGDocument unclipped = ParsePatternDocument(kUnclippedSvg);
    SVGDocument document = ParsePatternDocument(kClipEditSvg);
    const auto [contentCounters, contentBitmap] = steadyCounters(unclipped);
    const auto [counters, bitmap] = steadyCounters(document);
    if (bound.cacheBytes == 0u) {
      EXPECT_THAT(counters.pathEncodes, testing::Eq(contentCounters.pathEncodes + kClipShapes))
          << "Every clip shape is encoded again each frame once the cache budget refuses it";
    } else {
      // Arena uploads are flushed once per arena, so the arena fills show in their bind groups.
      EXPECT_THAT(counters.bindgroupCreates,
                  testing::Ge(contentCounters.bindgroupCreates + kClipShapes))
          << "Clip shapes fill from the per-frame arena, each with a bind group of its own, once "
             "the resident budget refuses them";
    }
    editor::tests::CompareBitmapToBitmap(bitmap, expected,
                                         std::string("clip_past_budget_") + bound.name,
                                         editor::tests::PixelmatchIdentityParams());
  }
}

/// A concurrent-DOM document is drawn by recording a snapshot and replaying it, and a snapshot
/// outlives its document, so the replay keeps no clip state on the live document: it caches no
/// clip geometry there and paints what a direct draw of the same document paints.
TEST_F(RendererGeodeTest, SnapshotReplayKeepsNoClipStateOnTheDocument) {
  ASSERT_THAT(sharedDevice(), testing::NotNull());
  SVGDocument direct = ParsePatternDocument(kClipEditSvg);
  SVGDocument replayed = ParsePatternDocument(kClipEditSvg);
  replayed.setThreadingMode(ThreadingMode::ConcurrentDom);

  RendererGeode directRenderer = createRenderer();
  directRenderer.draw(direct);
  directRenderer.draw(direct);
  ASSERT_THAT(directRenderer.clipGeometryCacheCountForTesting(direct), testing::Gt(0u))
      << "Premise: a direct draw caches clip geometry on the document";

  // The driver's concurrent-DOM draw records a snapshot under document access and replays it
  // without holding access.
  RendererGeode replayRenderer = createRenderer();
  RendererDriver driver(replayRenderer);
  driver.draw(replayed);
  driver.draw(replayed);
  std::size_t cachedClips = 0;
  {
    DocumentWriteAccess access = replayed.writeAccess();
    cachedClips = replayRenderer.clipGeometryCacheCountForTesting(replayed);
  }
  EXPECT_THAT(cachedClips, testing::Eq(0u))
      << "A snapshot replay must not write clip state into the live document";
  editor::tests::CompareBitmapToBitmap(replayRenderer.takeSnapshot(), directRenderer.takeSnapshot(),
                                       "clip_snapshot_replay",
                                       editor::tests::PixelmatchIdentityParams());
}

/// Cached clip geometry goes with the computed clip paths it was built from: once the clipped
/// elements stop referencing their clips and the document is drawn again, no entity keeps cached
/// clip geometry or its charge to the document's budgets.
TEST_F(RendererGeodeTest, ClipGeometryCacheGoesWithItsClip) {
  ASSERT_THAT(sharedDevice(), testing::NotNull());
  SVGDocument document = ParsePatternDocument(kClipEditSvg);
  RendererGeode renderer = createRenderer();
  renderer.draw(document);
  ASSERT_THAT(renderer.clipGeometryCacheCountForTesting(document), testing::Gt(0u))
      << "Premise: the clipped elements cache their clip geometry";

  for (const char* selector : {"#clipped", "#obb-target", "#nested-target", "#rule-target"}) {
    std::optional<SVGElement> element = document.querySelector(selector);
    ASSERT_THAT(element, testing::Optional(testing::_)) << selector;
    element->removeAttribute("clip-path");
  }
  renderer.draw(document);
  EXPECT_THAT(renderer.clipGeometryCacheCountForTesting(document), testing::Eq(0u))
      << "Clip geometry must not outlive the computed clip paths it was built from";
}

/// A renderer keeps a view of each clip-mask texture only while its frames keep using that
/// texture, so the views it holds are those of its last two frames. The mask textures themselves
/// go back to the device's pool at every frame end, whose byte budget bounds them.
TEST_F(RendererGeodeTest, ClipMaskViewsStayBoundedBetweenFrames) {
  ASSERT_THAT(sharedDevice(), testing::NotNull());
  RendererGeode renderer = createRenderer();
  const auto drawFrame = [&](int clipCount) {
    beginFrame(renderer);
    renderer.setPaint(solidFill(css::RGBA(0, 0, 255, 255)));
    for (int index = 0; index < clipCount; ++index) {
      ResolvedClip clip;
      clip.clipPaths.push_back(
          {.path = PathBuilder().addRect(Box2d::FromXYWH(index, index, 16.0, 16.0)).build()});
      renderer.pushClip(clip);
      renderer.drawRect(Box2d::FromXYWH(0.0, 0.0, kViewportSize, kViewportSize), StrokeParams{});
      renderer.popClip();
    }
    renderer.endFrame();
  };

  constexpr int kClipsPerFrame = 12;
  for (int frame = 0; frame < 4; ++frame) {
    drawFrame(kClipsPerFrame);
    EXPECT_THAT(renderer.clipMaskViewCountForTesting(),
                testing::AllOf(testing::Gt(0u), testing::Le(2u * kClipsPerFrame)))
        << "Views are kept for the current and the previous frame's mask textures only";
  }
  drawFrame(0);
  drawFrame(0);
  EXPECT_THAT(renderer.clipMaskViewCountForTesting(), testing::Eq(0u))
      << "Two frames without a clip leave no clip-mask view behind";
}

/// A clip shape's cached encode serves only the tolerance it was made at. A renderer that drew a
/// curved clip shape at one scale encodes it again when it draws it at a much larger one, as a
/// renderer that only ever drew the larger scale does, and renders the same pixels. Document edits
/// rebuild the computed clip paths and so drop the cache; this changes only the transform, which
/// keeps it.
TEST_F(RendererGeodeTest, ClipShapeCacheFollowsTheDeviceScale) {
  ASSERT_THAT(sharedDevice(), testing::NotNull());
  // The cache lives on the document, so the fresh renderer draws a document of its own.
  const auto parseDocument = [] {
    ParseWarningSink warnings = ParseWarningSink::Disabled();
    auto parsed = parser::SVGParser::ParseSVG(
        R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
          <clipPath id="c"><circle cx="0.5" cy="0.5" r="0.4"/></clipPath>
          <rect id="t" width="1" height="1" clip-path="url(#c)"/>
        </svg>)svg",
        warnings);
    EXPECT_THAT(parsed, NoParseError());
    return std::move(parsed).result();
  };
  const auto clipOn = [](SVGDocument& document) {
    ResolvedClip clip;
    clip.clipPaths.push_back({.path = PathBuilder()
                                          .moveTo({0.1, 0.5})
                                          .curveTo({0.1, 0.0}, {0.9, 0.0}, {0.9, 0.5})
                                          .curveTo({0.9, 1.0}, {0.1, 1.0}, {0.1, 0.5})
                                          .closePath()
                                          .build()});
    std::optional<SVGElement> target = document.querySelector("#t");
    EXPECT_THAT(target, testing::Optional(testing::_));
    if (target.has_value()) {
      clip.sourceEntity = target->unsafeEntityHandle();
    }
    return clip;
  };
  const auto drawAtScale = [&](RendererGeode& renderer, const ResolvedClip& clip, double scale) {
    beginFrame(renderer);
    renderer.setTransform(Transform2d::Scale(scale));
    renderer.pushClip(clip);
    renderer.setPaint(solidFill(css::RGBA(30, 90, 200, 255)));
    renderer.drawRect(Box2d::FromXYWH(0.0, 0.0, 1.0, 1.0), StrokeParams{});
    renderer.popClip();
    renderer.endFrame();
    return renderer.lastFrameTimings().counters.pathEncodes;
  };

  SVGDocument warmDocument = parseDocument();
  const ResolvedClip warmClip = clipOn(warmDocument);
  RendererGeode warm = createRenderer();
  drawAtScale(warm, warmClip, 1.0);
  const uint64_t sameScaleEncodes = drawAtScale(warm, warmClip, 1.0);
  const uint64_t newScaleEncodes = drawAtScale(warm, warmClip, 64.0);

  SVGDocument freshDocument = parseDocument();
  const ResolvedClip freshClip = clipOn(freshDocument);
  RendererGeode fresh = createRenderer();
  const uint64_t freshEncodes = drawAtScale(fresh, freshClip, 64.0);
  // The rectangle has no source entity, so every frame encodes it; only the clip shape caches.
  EXPECT_THAT(newScaleEncodes, testing::Eq(freshEncodes))
      << "At a new scale the clip shape is encoded again, as a fresh renderer encodes it";
  EXPECT_THAT(sameScaleEncodes, testing::Eq(freshEncodes - 1u))
      << "At an unchanged scale the clip shape draws from its cached encode";
  const RendererBitmap expected = fresh.takeSnapshot();
  ASSERT_THAT(test::CountNonTransparentPixels(expected), testing::Gt(0u));
  editor::tests::CompareBitmapToBitmap(warm.takeSnapshot(), expected, "clip_cache_device_scale",
                                       editor::tests::PixelmatchIdentityParams());
}

/// A clip shape whose path changes to another of the same command and point counts and the same
/// encoded size is encoded again and its resident geometry uploaded again: the new encode takes
/// the old one's storage, so only the encode's identity tells the slot that its geometry is stale.
TEST_F(RendererGeodeTest, ClipShapeCacheFollowsAPathOfTheSameSize) {
  ASSERT_THAT(sharedDevice(), testing::NotNull());
  const auto parseDocument = [] {
    ParseWarningSink warnings = ParseWarningSink::Disabled();
    auto parsed = parser::SVGParser::ParseSVG(
        R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
          <clipPath id="c"><rect width="8" height="8"/></clipPath>
          <rect id="t" width="64" height="64" clip-path="url(#c)"/>
        </svg>)svg",
        warnings);
    EXPECT_THAT(parsed, NoParseError());
    return std::move(parsed).result();
  };
  // The second triangle is the first moved by whole pixels, so their encodes have the same size.
  const Path first =
      PathBuilder().moveTo({8, 8}).lineTo({40, 12}).lineTo({20, 40}).closePath().build();
  const Path second =
      PathBuilder().moveTo({24, 20}).lineTo({56, 24}).lineTo({36, 52}).closePath().build();
  const auto drawClippedBy = [&](RendererGeode& renderer, SVGDocument& document, const Path& path) {
    ResolvedClip clip;
    clip.clipPaths.push_back({.path = path});
    std::optional<SVGElement> target = document.querySelector("#t");
    EXPECT_THAT(target, testing::Optional(testing::_));
    if (target.has_value()) {
      clip.sourceEntity = target->unsafeEntityHandle();
    }
    beginFrame(renderer);
    renderer.pushClip(clip);
    renderer.setPaint(solidFill(css::RGBA(30, 90, 200, 255)));
    renderer.drawRect(Box2d::FromXYWH(0.0, 0.0, kViewportSize, kViewportSize), StrokeParams{});
    renderer.popClip();
    renderer.endFrame();
  };

  SVGDocument warmDocument = parseDocument();
  RendererGeode warm = createRenderer();
  drawClippedBy(warm, warmDocument, first);
  drawClippedBy(warm, warmDocument, first);
  drawClippedBy(warm, warmDocument, second);
  SVGDocument freshDocument = parseDocument();
  RendererGeode fresh = createRenderer();
  drawClippedBy(fresh, freshDocument, second);
  const RendererBitmap expected = fresh.takeSnapshot();
  ASSERT_THAT(test::CountNonTransparentPixels(expected), testing::Gt(0u));
  editor::tests::CompareBitmapToBitmap(warm.takeSnapshot(), expected, "clip_cache_same_size_path",
                                       editor::tests::PixelmatchIdentityParams());
}

/// A cached clip shape intersected with a nested clip binds the nested clip's mask, and a frame
/// whose nested mask is another texture rebinds it. Here the target grows between frames, so
/// every clip mask is a texture of the new size, and the nested rectangle moves, so the old
/// nested mask no longer holds the coverage the outer shape must be intersected with; the outer
/// shape keeps its cached encode throughout.
TEST_F(RendererGeodeTest, ClipShapeRebindsItsNestedMask) {
  ASSERT_THAT(sharedDevice(), testing::NotNull());
  const auto parseDocument = [] {
    ParseWarningSink warnings = ParseWarningSink::Disabled();
    auto parsed = parser::SVGParser::ParseSVG(
        R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
          <clipPath id="c"><rect width="8" height="8"/></clipPath>
          <rect id="t" width="64" height="64" clip-path="url(#c)"/>
        </svg>)svg",
        warnings);
    EXPECT_THAT(parsed, NoParseError());
    return std::move(parsed).result();
  };
  // The circle is clipped by the nested rectangle \p nested.
  const auto clipOn = [](SVGDocument& document, const Box2d& nested) {
    ResolvedClip clip;
    clip.clipPaths.push_back({.path = PathBuilder().addRect(nested).build(), .layer = 1});
    clip.clipPaths.push_back(
        {.path = PathBuilder().addEllipse(Box2d::FromXYWH(8, 8, 48, 48)).build(), .layer = 0});
    std::optional<SVGElement> target = document.querySelector("#t");
    EXPECT_THAT(target, testing::Optional(testing::_));
    if (target.has_value()) {
      clip.sourceEntity = target->unsafeEntityHandle();
    }
    return clip;
  };
  const auto drawAtSize = [&](RendererGeode& renderer, const ResolvedClip& clip, double size) {
    RenderViewport viewport;
    viewport.size = Vector2d(size, size);
    viewport.devicePixelRatio = 1.0;
    renderer.beginFrame(viewport);
    renderer.pushClip(clip);
    renderer.setPaint(solidFill(css::RGBA(30, 90, 200, 255)));
    renderer.drawRect(Box2d::FromXYWH(0.0, 0.0, size, size), StrokeParams{});
    renderer.popClip();
    renderer.endFrame();
  };
  const Box2d leftPart = Box2d::FromXYWH(4, 4, 24, 56);
  const Box2d rightPart = Box2d::FromXYWH(36, 4, 24, 56);

  SVGDocument warmDocument = parseDocument();
  RendererGeode warm = createRenderer();
  drawAtSize(warm, clipOn(warmDocument, leftPart), 64.0);
  drawAtSize(warm, clipOn(warmDocument, leftPart), 64.0);
  drawAtSize(warm, clipOn(warmDocument, rightPart), 96.0);
  SVGDocument freshDocument = parseDocument();
  RendererGeode fresh = createRenderer();
  drawAtSize(fresh, clipOn(freshDocument, rightPart), 96.0);
  const RendererBitmap expected = fresh.takeSnapshot();
  ASSERT_THAT(test::CountNonTransparentPixels(expected), testing::Gt(0u));
  editor::tests::CompareBitmapToBitmap(warm.takeSnapshot(), expected, "clip_nested_mask_rebind",
                                       editor::tests::PixelmatchIdentityParams());
}

/// An offscreen pass that builds a clip while an outer frame on the same device has recorded a
/// mask fill of that clip, and not submitted it, leaves the outer fill as it was recorded. The
/// clip has a curved shape, which the nested pass, drawing at another scale, encodes again into
/// the shape's slot, and a polygon, whose encode serves every scale, so the nested pass draws
/// the slot the outer frame already drew.
TEST_F(RendererGeodeTest, NestedPassKeepsTheClipMaskAnOuterFrameLeftPending) {
  ASSERT_THAT(sharedDevice(), testing::NotNull());
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
        <clipPath id="c"><circle cx="20" cy="20" r="14"/></clipPath>
        <rect id="t" width="64" height="64" fill="red" clip-path="url(#c)"/>
      </svg>)svg",
      warnings);
  ASSERT_THAT(parsed, NoParseError());
  SVGDocument document = std::move(parsed).result();
  auto target = document.querySelector("#t");
  ASSERT_THAT(target, testing::Optional(testing::_));

  ResolvedClip clip;
  clip.clipPaths.push_back(
      {.path =
           PathBuilder().moveTo({8, 40}).curveTo({20, 8}, {44, 8}, {56, 40}).closePath().build()});
  clip.clipPaths.push_back(
      {.path =
           PathBuilder().moveTo({30, 34}).lineTo({60, 40}).lineTo({40, 60}).closePath().build()});
  ResolvedClip cachedClip = clip;
  cachedClip.sourceEntity = target->unsafeEntityHandle();
  const css::RGBA red(200, 0, 0, 255);
  const auto drawClipped = [&](RendererInterface& renderer, const ResolvedClip& withClip) {
    renderer.pushClip(withClip);
    renderer.setPaint(solidFill(red));
    renderer.drawRect(Box2d::FromXYWH(0.0, 0.0, kViewportSize, kViewportSize), StrokeParams{});
    renderer.popClip();
  };

  RendererGeode reference = createRenderer();
  beginFrame(reference);
  drawClipped(reference, clip);
  reference.endFrame();
  const RendererBitmap expected = reference.takeSnapshot();
  ASSERT_THAT(test::CountNonTransparentPixels(expected), testing::Gt(0u));

  RendererGeode outer = createRenderer();
  beginFrame(outer);
  drawClipped(outer, cachedClip);

  std::unique_ptr<RendererInterface> inner = outer.createOffscreenInstance();
  ASSERT_THAT(inner, testing::NotNull());
  RenderViewport viewport;
  viewport.size = Vector2d(kViewportSize, kViewportSize);
  viewport.devicePixelRatio = 1.0;
  inner->beginFrame(viewport);
  for (const double scale : {0.5, 4.0}) {
    inner->setTransform(Transform2d::Scale(scale));
    drawClipped(*inner, cachedClip);
  }
  inner->endFrame();

  outer.endFrame();
  editor::tests::CompareBitmapToBitmap(outer.takeSnapshot(), expected,
                                       "nested_pass_keeps_pending_clip_mask",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST_F(RendererGeodeTest, TransientTexturePoolStaysWithinGlobalMemoryBudgetAcrossSizeChurn) {
  constexpr int kUniqueTextureSizes = 122;
  constexpr int kMinimumTextureDimension = 512;
  constexpr uint64_t kExpectedBudgetBytes = 64u * 1024u * 1024u;
  constexpr std::size_t kMaximumTextureCount =
      kExpectedBudgetBytes / (kMinimumTextureDimension * kMinimumTextureDimension * 4u);

  RendererGeode firstRenderer = createRenderer();
  RendererGeode secondRenderer = createRenderer();
  for (int index = 0; index < kUniqueTextureSizes; ++index) {
    RendererGeode& renderer = index % 2 == 0 ? firstRenderer : secondRenderer;
    RenderViewport viewport;
    viewport.size = Vector2d(kMinimumTextureDimension + index, kMinimumTextureDimension);
    viewport.devicePixelRatio = 1.0;
    renderer.beginFrame(viewport);
    renderer.pushIsolatedLayer(1.0, MixBlendMode::Normal);
    renderer.popIsolatedLayer();
    renderer.endFrame();
  }

  RenderViewport crossRendererReuseViewport;
  crossRendererReuseViewport.size =
      Vector2d(kMinimumTextureDimension + kUniqueTextureSizes - 2, kMinimumTextureDimension);
  crossRendererReuseViewport.devicePixelRatio = 1.0;
  secondRenderer.beginFrame(crossRendererReuseViewport);
  secondRenderer.pushIsolatedLayer(1.0, MixBlendMode::Normal);
  secondRenderer.popIsolatedLayer();
  secondRenderer.endFrame();
  EXPECT_EQ(secondRenderer.lastFrameTimings().counters.textureCreates, 1u)
      << "Only the resized primary target should be created; the isolated-layer texture was "
         "released by the other renderer and must be reused";

  const RendererGeodeTexturePoolStats firstStats = firstRenderer.texturePoolStats();
  const RendererGeodeTexturePoolStats secondStats = secondRenderer.texturePoolStats();
  EXPECT_EQ(firstStats.bytes, secondStats.bytes)
      << "Renderers sharing a GeodeDevice must report the same device-global pool";
  EXPECT_EQ(firstStats.textureCount, secondStats.textureCount);
  EXPECT_EQ(firstStats.bucketCount, secondStats.bucketCount);
  EXPECT_EQ(firstStats.budgetBytes, secondStats.budgetBytes);
  EXPECT_EQ(firstStats.budgetBytes, kExpectedBudgetBytes);
  EXPECT_LE(firstStats.bytes, kExpectedBudgetBytes)
      << "Size churn must not retain unbounded free GPU texture memory";
  EXPECT_LE(firstStats.textureCount, kMaximumTextureCount)
      << "Each exercised texture is at least 1 MiB, so the byte budget also bounds count";
  EXPECT_LE(firstStats.bucketCount, kMaximumTextureCount)
      << "Unique texture sizes must not bypass the global pool budget";
}

TEST_F(RendererGeodeTest, SurfaceBudgetCountsPoolReuseAndNestedOffscreensBeforeAllocation) {
  ASSERT_TRUE(sharedDevice() != nullptr);

  RendererGeode renderer = createRenderer();
  renderer.setSurfaceBudgetForTesting(/*maximumSurfaces=*/3u, /*maximumBytes=*/4096u);
  std::unique_ptr<RendererInterface> offscreen = renderer.createOffscreenInstance();
  ASSERT_NE(offscreen, nullptr);

  RenderViewport viewport;
  viewport.size = Vector2d(8.0, 8.0);
  renderer.beginFrame(viewport);
  offscreen->beginFrame(viewport);
  renderer.pushIsolatedLayer(1.0, MixBlendMode::Normal);
  offscreen->pushIsolatedLayer(1.0, MixBlendMode::Normal);

  const RendererResourceStats stats = renderer.resourceStats();
  EXPECT_TRUE(stats.surfaceBudgetSupported);
  EXPECT_EQ(stats.surfaceCount, 3u);
  EXPECT_EQ(stats.surfaceBytes, 3u * 8u * 8u * 4u);
  EXPECT_TRUE(stats.surfaceBudgetRejected);

  offscreen->popIsolatedLayer();
  renderer.popIsolatedLayer();
  offscreen->endFrame();
  renderer.endFrame();
}

TEST_F(RendererGeodeTest, SurfaceBudgetCoversClipMaskBlendSnapshotMaskAndPatternTargets) {
  ASSERT_TRUE(sharedDevice() != nullptr);
  RenderViewport viewport;
  viewport.size = Vector2d(8.0, 8.0);

  {
    RendererGeode renderer = createRenderer();
    renderer.setSurfaceBudgetForTesting(/*maximumSurfaces=*/2u, /*maximumBytes=*/4096u);
    renderer.beginFrame(viewport);
    ResolvedClip clip;
    clip.clipPaths.push_back(
        {.path = PathBuilder().addRect(Box2d({0.0, 0.0}, {4.0, 4.0})).build()});
    renderer.pushClip(clip);
    EXPECT_EQ(renderer.resourceStats().surfaceCount, 2u);
    renderer.popClip();
    renderer.endFrame();
  }

  {
    RendererGeode renderer = createRenderer();
    renderer.setSurfaceBudgetForTesting(/*maximumSurfaces=*/3u, /*maximumBytes=*/4096u);
    renderer.beginFrame(viewport);
    renderer.pushIsolatedLayer(1.0, MixBlendMode::Multiply);
    renderer.popIsolatedLayer();
    EXPECT_EQ(renderer.resourceStats().surfaceCount, 3u)
        << "The layer and its destination snapshot must both be admitted.";
    renderer.endFrame();
  }

  {
    RendererGeode renderer = createRenderer();
    renderer.setSurfaceBudgetForTesting(/*maximumSurfaces=*/3u, /*maximumBytes=*/4096u);
    renderer.beginFrame(viewport);
    renderer.pushMask(std::nullopt, MaskType::Alpha);
    renderer.transitionMaskToContent();
    EXPECT_EQ(renderer.resourceStats().surfaceCount, 3u)
        << "Mask capture and content surfaces must be charged independently.";
    renderer.popMask();
    renderer.endFrame();
  }

  {
    RendererGeode renderer = createRenderer();
    renderer.setSurfaceBudgetForTesting(/*maximumSurfaces=*/2u, /*maximumBytes=*/4096u);
    renderer.beginFrame(viewport);
    ASSERT_TRUE(renderer.beginPatternTile(Box2d::FromXYWH(0.0, 0.0, 4.0, 4.0), Transform2d()));
    EXPECT_EQ(renderer.resourceStats().surfaceCount, 2u);
    renderer.endPatternTile(/*forStroke=*/false, std::nullopt);
    renderer.endFrame();
  }
}

TEST_F(RendererGeodeTest, GeometryBudgetIsSharedAcrossOffscreensAtExactDrawBoundary) {
  ASSERT_TRUE(sharedDevice() != nullptr);

  RendererGeode renderer = createRenderer();
  renderer.setGeometryBudgetForTesting(/*maximumDraws=*/2u, /*maximumItems=*/1u << 20,
                                       /*maximumFrameBytes=*/64u << 20,
                                       /*maximumCacheBytes=*/64u << 20,
                                       /*maximumResidentBytes=*/64u << 20);
  std::unique_ptr<RendererInterface> offscreen = renderer.createOffscreenInstance();
  ASSERT_NE(offscreen, nullptr);

  RenderViewport viewport;
  viewport.size = Vector2d(16.0, 16.0);
  renderer.beginFrame(viewport);
  offscreen->beginFrame(viewport);
  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  offscreen->setPaint(solidFill(css::RGBA(0, 255, 0, 255)));
  renderer.drawRect(Box2d({0.0, 0.0}, {4.0, 4.0}), StrokeParams{});
  offscreen->drawRect(Box2d({4.0, 0.0}, {8.0, 4.0}), StrokeParams{});
  renderer.drawRect(Box2d({8.0, 0.0}, {12.0, 4.0}), StrokeParams{});

  const RendererResourceStats stats = renderer.resourceStats();
  EXPECT_TRUE(stats.geometryBudgetSupported);
  EXPECT_EQ(stats.geometryDraws, 2u);
  EXPECT_TRUE(stats.geometryBudgetRejected);

  offscreen->endFrame();
  renderer.endFrame();
}

TEST_F(RendererGeodeTest, GeometryBudgetChargesFillAndStrokeVariantsBeforeUpload) {
  ASSERT_TRUE(sharedDevice() != nullptr);

  RendererGeode renderer = createRenderer();
  renderer.setGeometryBudgetForTesting(/*maximumDraws=*/2u, /*maximumItems=*/1u << 20,
                                       /*maximumFrameBytes=*/64u << 20,
                                       /*maximumCacheBytes=*/64u << 20,
                                       /*maximumResidentBytes=*/64u << 20);
  beginFrame(renderer);
  renderer.setPaint(solidFillAndStroke(css::RGBA(255, 0, 0, 255), css::RGBA(0, 0, 255, 255)));
  StrokeParams stroke;
  stroke.strokeWidth = 1.0;
  renderer.drawRect(Box2d({1.0, 1.0}, {8.0, 8.0}), stroke);

  RendererResourceStats stats = renderer.resourceStats();
  EXPECT_EQ(stats.geometryDraws, 2u);
  EXPECT_FALSE(stats.geometryBudgetRejected);

  renderer.drawRect(Box2d({10.0, 1.0}, {18.0, 8.0}), stroke);
  stats = renderer.resourceStats();
  EXPECT_EQ(stats.geometryDraws, 2u);
  EXPECT_TRUE(stats.geometryBudgetRejected);
  renderer.endFrame();
}

TEST_F(RendererGeodeTest, StrokeOnlyDrawDoesNotPayForANonexistentFill) {
  ASSERT_TRUE(sharedDevice() != nullptr);

  RendererGeode renderer = createRenderer();
  renderer.setGeometryBudgetForTesting(/*maximumDraws=*/1u, /*maximumItems=*/1u << 20,
                                       /*maximumFrameBytes=*/64u << 20,
                                       /*maximumCacheBytes=*/64u << 20,
                                       /*maximumResidentBytes=*/64u << 20);
  beginFrame(renderer);
  renderer.setPaint(solidStroke(css::RGBA(0, 0, 255, 255)));
  StrokeParams stroke;
  stroke.strokeWidth = 2.0;
  renderer.drawRect(Box2d({2.0, 2.0}, {12.0, 12.0}), stroke);

  const RendererResourceStats stats = renderer.resourceStats();
  EXPECT_EQ(stats.geometryDraws, 1u);
  EXPECT_FALSE(stats.geometryBudgetRejected);
  renderer.endFrame();

  const RendererBitmap bitmap = renderer.takeSnapshot();
  ASSERT_FALSE(bitmap.empty());
  EXPECT_THAT(PixelAt(bitmap, 2, 2), testing::Not(IsTransparent()));
}

TEST_F(RendererGeodeTest, DegenerateEmptyGeometryDoesNotConsumeFrameBudget) {
  ASSERT_TRUE(sharedDevice() != nullptr);

  RendererGeode renderer = createRenderer();
  renderer.setGeometryBudgetForTesting(/*maximumDraws=*/1u, /*maximumItems=*/1u << 20,
                                       /*maximumFrameBytes=*/64u << 20,
                                       /*maximumCacheBytes=*/64u << 20,
                                       /*maximumResidentBytes=*/64u << 20);
  beginFrame(renderer);
  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  renderer.drawRect(Box2d({1.0, 1.0}, {4.0, 4.0}), StrokeParams{});
  const Path empty;
  renderer.drawPath(PathShape{.path = &empty}, StrokeParams{});

  const RendererResourceStats stats = renderer.resourceStats();
  EXPECT_EQ(stats.geometryDraws, 1u);
  EXPECT_FALSE(stats.geometryBudgetRejected);
  renderer.endFrame();
}

TEST_F(RendererGeodeTest, ClipMaskGeometryUsesTheSharedSubmissionBudget) {
  ASSERT_TRUE(sharedDevice() != nullptr);

  RendererGeode renderer = createRenderer();
  renderer.setGeometryBudgetForTesting(/*maximumDraws=*/1u, /*maximumItems=*/1u << 20,
                                       /*maximumFrameBytes=*/64u << 20,
                                       /*maximumCacheBytes=*/64u << 20,
                                       /*maximumResidentBytes=*/64u << 20);
  beginFrame(renderer);
  ResolvedClip clip;
  clip.clipPaths.push_back(
      {.path = PathBuilder().addRect(Box2d({0.0, 0.0}, {8.0, 8.0})).build(), .layer = 0});
  clip.clipPaths.push_back(
      {.path = PathBuilder().addRect(Box2d({8.0, 8.0}, {16.0, 16.0})).build(), .layer = 0});
  clip.clipPaths.push_back(
      {.path = PathBuilder().addRect(Box2d({4.0, 4.0}, {12.0, 12.0})).build(), .layer = 0});
  renderer.pushClip(clip);

  const RendererResourceStats stats = renderer.resourceStats();
  EXPECT_EQ(stats.geometryDraws, 1u);
  EXPECT_TRUE(stats.geometryBudgetRejected);
  EXPECT_EQ(renderer.lastFrameTimings().counters.pathEncodes, 2u)
      << "Once cap+1 latches rejection, later mask shapes must skip CPU encoding.";
  renderer.popClip();
  renderer.endFrame();
}

TEST_F(RendererGeodeTest, NestedClipSurfaceRejectionSuppressesTheWholeInnerSubtree) {
  ASSERT_TRUE(sharedDevice() != nullptr);

  RendererGeode renderer = createRenderer();
  renderer.setSurfaceBudgetForTesting(/*maximumSurfaces=*/2u, /*maximumBytes=*/4096u);
  RenderViewport viewport;
  viewport.size = Vector2d(16.0, 16.0);
  renderer.beginFrame(viewport);

  ResolvedClip outer;
  outer.clipPaths.push_back(
      {.path = PathBuilder().addRect(Box2d({0.0, 0.0}, {16.0, 16.0})).build(), .layer = 0});
  renderer.pushClip(outer);
  ResolvedClip rejectedInner;
  rejectedInner.clipPaths.push_back(
      {.path = PathBuilder().addRect(Box2d({4.0, 4.0}, {12.0, 12.0})).build(), .layer = 0});
  renderer.pushClip(rejectedInner);

  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  renderer.drawRect(Box2d({0.0, 0.0}, {16.0, 16.0}), StrokeParams{});
  EXPECT_TRUE(renderer.resourceStats().surfaceBudgetRejected);
  renderer.popClip();
  renderer.popClip();
  renderer.endFrame();

  const RendererBitmap bitmap = renderer.takeSnapshot();
  ASSERT_FALSE(bitmap.empty());
  EXPECT_THAT(bitmap.pixels, testing::Each(0u))
      << "A clip whose required mask could not be allocated must fail closed.";
}

TEST_F(RendererGeodeTest, ResourceStatsAggregateEveryDocumentTouchedInTheFrame) {
  ASSERT_TRUE(sharedDevice() != nullptr);

  Registry firstRegistry;
  const Entity firstEntity = firstRegistry.create();
  PathBuilder largeBuilder;
  largeBuilder.moveTo({0.0, 0.0});
  for (int i = 1; i <= 512; ++i) {
    largeBuilder.lineTo({static_cast<double>(i % 16), static_cast<double>(i / 16)});
  }
  const Path largePath = largeBuilder.build();

  Registry secondRegistry;
  const Entity secondEntity = secondRegistry.create();
  const Path smallPath = PathBuilder().addRect(Box2d({1.0, 1.0}, {4.0, 4.0})).build();

  RendererGeode renderer = createRenderer();
  renderer.setGeometryBudgetForTesting(/*maximumDraws=*/4u, /*maximumItems=*/1u << 20,
                                       /*maximumFrameBytes=*/64u << 20,
                                       /*maximumCacheBytes=*/4096u,
                                       /*maximumResidentBytes=*/64u << 20);
  beginFrame(renderer);
  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  renderer.drawPath(
      PathShape{.path = &largePath, .sourceEntity = EntityHandle(firstRegistry, firstEntity)},
      StrokeParams{});
  renderer.drawPath(
      PathShape{.path = &smallPath, .sourceEntity = EntityHandle(secondRegistry, secondEntity)},
      StrokeParams{});

  const auto* firstCache = firstRegistry.try_get<geode::GeodePathCacheComponent>(firstEntity);
  const auto* secondCache = secondRegistry.try_get<geode::GeodePathCacheComponent>(secondEntity);
  ASSERT_NE(firstCache, nullptr);
  ASSERT_NE(secondCache, nullptr);
  ASSERT_FALSE(firstCache->fillVariants[0].encode.has_value())
      << "Fixture must reject the first document.";
  ASSERT_TRUE(secondCache->fillVariants[0].encode.has_value())
      << "Fixture must admit the second document.";
  EXPECT_TRUE(renderer.resourceStats().geometryBudgetRejected)
      << "Touching a clean second document must not erase the first document's rejection.";
  renderer.endFrame();
}

TEST_F(RendererGeodeTest, SingletonConversionFailureRefundsCurrentSceneAdmission) {
  ASSERT_TRUE(sharedDevice() != nullptr);

  Registry firstRegistry;
  const Entity firstEntity = firstRegistry.create();
  Registry secondRegistry;
  const Entity secondEntity = secondRegistry.create();
  const Path firstPath = PathBuilder().addRect(Box2d({1.0, 1.0}, {6.0, 6.0})).build();
  const Path secondPath = PathBuilder().addRect(Box2d({10.0, 1.0}, {15.0, 6.0})).build();

  RendererGeode renderer = createRenderer();
  renderer.setGeometryBudgetForTesting(/*maximumDraws=*/2u, /*maximumItems=*/1u << 20,
                                       /*maximumFrameBytes=*/64u << 20,
                                       /*maximumCacheBytes=*/64u << 20,
                                       /*maximumResidentBytes=*/64u << 20);
  beginFrame(renderer);
  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  renderer.drawPath(
      PathShape{.path = &firstPath, .sourceEntity = EntityHandle(firstRegistry, firstEntity)},
      StrokeParams{});
  renderer.injectScenePreparationFailureAfterForTesting(/*successfulPreparations=*/1u);
  renderer.drawPath(
      PathShape{.path = &secondPath, .sourceEntity = EntityHandle(secondRegistry, secondEntity)},
      StrokeParams{});
  renderer.endFrame();

  const RendererResourceStats stats = renderer.resourceStats();
  EXPECT_EQ(stats.geometryDraws, 2u);
  EXPECT_FALSE(stats.geometryBudgetRejected)
      << "Demoting the prepared current draw must refund its scene token before solo replay.";
  const RendererBitmap bitmap = renderer.takeSnapshot();
  ASSERT_FALSE(bitmap.empty());
  EXPECT_THAT(PixelAt(bitmap, 3, 3), RgbaEq(255, 0, 0, 255));
  EXPECT_THAT(PixelAt(bitmap, 12, 3), RgbaEq(255, 0, 0, 255));
}

TEST_F(RendererGeodeTest, CacheBudgetRejectsBeforeInsertionAndUsesTransientFallback) {
  ASSERT_TRUE(sharedDevice() != nullptr);

  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">
           <path id="bounded" d="M 1 1 H 12 V 12 H 1 Z" fill="red"/>
         </svg>)",
      warnings);
  ASSERT_FALSE(parsed.hasError());
  SVGDocument document = std::move(parsed.result());
  const auto element = document.querySelector("#bounded");
  ASSERT_TRUE(element.has_value());
  const Entity entity = element->unsafeEntityHandle().entity();

  RendererGeode renderer = createRenderer();
  renderer.setGeometryBudgetForTesting(/*maximumDraws=*/4u, /*maximumItems=*/1u << 20,
                                       /*maximumFrameBytes=*/64u << 20,
                                       /*maximumCacheBytes=*/0u,
                                       /*maximumResidentBytes=*/64u << 20);
  renderer.draw(document);

  const auto* cache = document.registry().try_get<geode::GeodePathCacheComponent>(entity);
  ASSERT_NE(cache, nullptr);
  EXPECT_FALSE(cache->fillVariants[0].encode.has_value())
      << "Rejected retained geometry must not enter the document cache.";
  const RendererResourceStats stats = renderer.resourceStats();
  EXPECT_TRUE(stats.geometryBudgetRejected);
  EXPECT_EQ(stats.geometryDraws, 1u);

  const RendererBitmap bitmap = renderer.takeSnapshot();
  ASSERT_FALSE(bitmap.empty());
  EXPECT_THAT(PixelAt(bitmap, 4, 4), RgbaEq(255, 0, 0, 255))
      << "A cache rejection must retain the admitted frame-local draw fallback.";
}

TEST_F(RendererGeodeTest, DrawTextureSnapshotPreservesPremultipliedAlpha) {
  ASSERT_TRUE(sharedDevice() != nullptr);

  RendererGeode source = createRenderer();
  beginFrame(source);
  source.setPaint(solidFill(css::RGBA(255, 0, 0, 128)));
  source.drawRect(Box2d({16, 16}, {48, 48}), StrokeParams{});
  source.endFrame();

  std::shared_ptr<const RendererTextureSnapshot> texture = source.takeTextureSnapshot();
  ASSERT_TRUE(texture != nullptr);
  EXPECT_EQ(texture->alphaType(), AlphaType::Premultiplied);

  RendererGeode composited = createRenderer();
  beginFrame(composited);
  ASSERT_TRUE(composited.drawTextureSnapshot(
      *texture, Box2d(Vector2d::Zero(), Vector2d(kViewportSize, kViewportSize))));
  composited.endFrame();
  const RendererBitmap actual = composited.takeSnapshot();

  RendererGeode reference = createRenderer();
  beginFrame(reference);
  reference.setPaint(solidFill(css::RGBA(255, 0, 0, 128)));
  reference.drawRect(Box2d({16, 16}, {48, 48}), StrokeParams{});
  reference.endFrame();
  const RendererBitmap expected = reference.takeSnapshot();

  ASSERT_FALSE(actual.empty());
  ASSERT_FALSE(expected.empty());
  ASSERT_EQ(actual.dimensions, expected.dimensions);
  EXPECT_EQ(PixelAt(actual, 32, 32), PixelAt(expected, 32, 32))
      << "Texture snapshots are premultiplied render-target pixels. The blit shader must not "
         "premultiply them again.";
}

TEST_F(RendererGeodeTest, DrawTextureSnapshotHonorsCurrentTransform) {
  ASSERT_TRUE(sharedDevice() != nullptr);

  RendererGeode source = createRenderer();
  beginFrame(source);
  source.setPaint(solidFill(css::RGBA(0, 255, 0, 255)));
  source.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});
  source.endFrame();

  std::shared_ptr<const RendererTextureSnapshot> texture = source.takeTextureSnapshot();
  ASSERT_TRUE(texture != nullptr);

  RendererGeode composited = createRenderer();
  beginFrame(composited);
  composited.setTransform(Transform2d::Translate(Vector2d(24.0, 24.0)));
  ASSERT_TRUE(composited.drawTextureSnapshot(*texture, Box2d({0.0, 0.0}, {16.0, 16.0})));
  composited.endFrame();

  const RendererBitmap actual = composited.takeSnapshot();
  ASSERT_FALSE(actual.empty());
  EXPECT_THAT(PixelAt(actual, 8, 8), IsTransparent())
      << "The texture snapshot must be translated out of its local-space source rect.";
  EXPECT_THAT(PixelAt(actual, 32, 32), RgbaEq(0, 255, 0, 255))
      << "drawTextureSnapshot must apply the renderer's current transform like other draw calls.";
}

TEST_F(RendererGeodeTest, DrawTextureSnapshotHonorsCurrentTransformWithClip) {
  ASSERT_TRUE(sharedDevice() != nullptr);

  RendererGeode source = createRenderer();
  beginFrame(source);
  source.setPaint(solidFill(css::RGBA(0, 255, 0, 255)));
  source.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});
  source.endFrame();

  std::shared_ptr<const RendererTextureSnapshot> texture = source.takeTextureSnapshot();
  ASSERT_TRUE(texture != nullptr);

  RendererGeode composited = createRenderer();
  beginFrame(composited);
  ResolvedClip clip;
  clip.clipRect = Box2d({20.0, 20.0}, {48.0, 48.0});
  composited.pushClip(clip);
  composited.setTransform(Transform2d::Translate(Vector2d(24.0, 24.0)));
  ASSERT_TRUE(composited.drawTextureSnapshot(*texture, Box2d({0.0, 0.0}, {16.0, 16.0})));
  composited.popClip();
  composited.endFrame();

  const RendererBitmap actual = composited.takeSnapshot();
  ASSERT_FALSE(actual.empty());
  EXPECT_THAT(PixelAt(actual, 8, 8), IsTransparent());
  EXPECT_THAT(PixelAt(actual, 32, 32), RgbaEq(0, 255, 0, 255))
      << "Texture-snapshot presentation must still draw when the document-image clip is active.";
}

TEST_F(RendererGeodeTest, DrawEntityRangeInvalidatesCachedFillEncodeAfterPathMutation) {
  ParseWarningSink warningSink;
  auto maybeDocument = parser::SVGParser::ParseSVG(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
             <path id="p" d="M 10 10 L 80 10" style="fill: none; stroke: black; stroke-width: 1"/>
           </svg>)svg",
      warningSink);
  ASSERT_FALSE(maybeDocument.hasError()) << maybeDocument.error();
  SVGDocument document = std::move(maybeDocument).result();
  auto path = document.querySelector("#p");
  ASSERT_TRUE(path.has_value());
  const Entity pathEntity = path->unsafeEntityHandle().entity();

  RendererGeode renderer = createRenderer();
  const RendererBitmap before = drawPreparedEntityRange(renderer, document, pathEntity);
  ASSERT_FALSE(before.empty());
  EXPECT_THAT(PixelAt(before, 24, 24), IsTransparent());

  path->setAttribute("d", "M 10 10 L 80 10 L 10 80 Z");
  path->setAttribute("style", "fill: #00ff00; stroke: none");

  const RendererBitmap after = drawPreparedEntityRange(renderer, document, pathEntity);
  ASSERT_FALSE(after.empty());
  EXPECT_THAT(PixelAt(after, 24, 24),
              Rgba(testing::Lt(20), testing::Gt(220), testing::Lt(20), testing::Gt(220)))
      << "drawEntityRange must not reuse a Geode fill encode cached before the path's `d` "
         "attribute changed.";
}

TEST_F(RendererGeodeTest, CubicFillCacheBoundsShapeErrorAfterZoom) {
  ParseWarningSink warningSink;
  auto parsed = parser::SVGParser::ParseSVG(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
        <path id="p" d="M0 0 C.3333333333333333 0 .6666666666666666 0 1 1 L1 0 Z"/>
      </svg>)svg",
      warningSink);
  ASSERT_THAT(parsed.hasError(), testing::IsFalse());
  SVGDocument document = std::move(parsed).result();
  const auto path = document.querySelector("#p");
  ASSERT_THAT(path, testing::Optional(testing::_));
  const EntityHandle source = path->unsafeEntityHandle();
  RendererUtils::prepareDocumentForRendering(document, false, warningSink);

  RendererGeode renderer = createRenderer();
  const auto renderAtScale = [&](double scale) {
    RenderViewport viewport;
    viewport.size = Vector2d(64.0, 64.0);
    viewport.devicePixelRatio = 1.0;
    RendererDriver(renderer).drawEntityRange(document.registry(), source.entity(), source.entity(),
                                             viewport, Transform2d::Scale(scale));
  };
  const auto encodeAtScale = [&](double scale) -> const geode::EncodedPath* {
    const auto* cache = source.try_get<geode::GeodePathCacheComponent>();
    if (cache == nullptr) {
      return nullptr;
    }
    const std::optional<std::size_t> variant =
        cache->fillVariantFor(geode::FillCubicToleranceFor(Transform2d::Scale(scale)));
    return variant ? &*cache->fillVariants[*variant].encode : nullptr;
  };
  renderAtScale(1.0);
  const geode::EncodedPath* initialEncode = encodeAtScale(1.0);
  ASSERT_THAT(initialEncode, testing::NotNull());
  const size_t initialCurveCount = initialEncode->curves.size();

  renderAtScale(32.0);
  const geode::EncodedPath* zoomedEncode = encodeAtScale(32.0);
  ASSERT_THAT(zoomedEncode, testing::NotNull());
  EXPECT_THAT(zoomedEncode->curves.size(), testing::Gt(initialCurveCount));
  EXPECT_THAT(renderer.lastFrameTimings().counters.pathEncodes, testing::Eq(1u));

  // This cubic has x(t) = t and y(t) = t^3, so its shape has an exact reference.
  double maximumDeviceError = 0.0;
  for (const auto& curve : zoomedEncode->curves) {
    if (curve.p2x <= curve.p0x) {
      continue;
    }
    for (int sample = 0; sample <= 32; ++sample) {
      const double t = static_cast<double>(sample) / 32.0;
      const double u = 1.0 - t;
      const double x = u * u * curve.p0x + 2.0 * u * t * curve.p1x + t * t * curve.p2x;
      const double y = u * u * curve.p0y + 2.0 * u * t * curve.p1y + t * t * curve.p2y;
      maximumDeviceError = std::max(maximumDeviceError, 32.0 * std::abs(y - x * x * x));
    }
  }
  EXPECT_THAT(maximumDeviceError, testing::Le(0.1))
      << "The cached quadratic fill must remain within 0.1 device pixels of the cubic after zoom.";

  renderAtScale(25.0);
  EXPECT_THAT(renderer.lastFrameTimings().counters.pathEncodes, testing::Eq(0u))
      << "Zoom inside the same scale bucket must reuse the fill encode.";

  renderAtScale(1.0);
  EXPECT_THAT(renderer.lastFrameTimings().counters.pathEncodes, testing::Eq(0u))
      << "Zooming back out must reuse the encode still cached for the original bucket.";
  EXPECT_THAT(encodeAtScale(1.0)->curves.size(), testing::Eq(initialCurveCount))
      << "The identity-scale draw must use the identity-bucket encode, not the finer one.";
}

// One element drawn at two device scales in a frame must not make either draw depend on which
// scale encoded it first. The blur turns sub-level coverage differences between a coarse and a
// fine cubic encode into visible ones, so any history dependence shows up as differing pixels.
TEST_F(RendererGeodeTest, FillEncodeAtTwoScalesDoesNotDependOnHistory) {
  constexpr std::string_view kSprite = R"svg(
        <filter id="blur" x="-50%" y="-50%" width="200%" height="200%">
          <feGaussianBlur stdDeviation="2"/>
        </filter>
        <g id="sprite" filter="url(#blur)"><circle cx="16" cy="16" r="8" fill="#c040a0"/></g>)svg";
  const auto parse = [&](std::string_view uses) {
    ParseWarningSink warnings;
    const std::string source =
        std::string(
            R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="128" height="64"><defs>)svg") +
        std::string(kSprite) + "</defs>" + std::string(uses) + "</svg>";
    auto parsed = parser::SVGParser::ParseSVG(source, warnings);
    EXPECT_THAT(parsed.hasError(), testing::IsFalse());
    return std::move(parsed).result();
  };
  // Both copies draw the same circle entity. The magnified copy lands in a finer tolerance
  // bucket than the identity-scale copy, which draws first.
  constexpr std::string_view kSingleUse = R"svg(<use href="#sprite"/>)svg";
  const std::string twoScales =
      std::string(kSingleUse) +
      R"svg(<use href="#sprite" transform="translate(64 0) scale(1.5)"/>)svg";

  SVGDocument single = parse(kSingleUse);
  RendererGeode singleRenderer = createRenderer();
  singleRenderer.draw(single);
  const RendererBitmap singleFrame = singleRenderer.takeSnapshot();
  ASSERT_FALSE(singleFrame.empty());

  SVGDocument document = parse(twoScales);
  RendererGeode renderer = createRenderer();
  std::vector<RendererBitmap> frames;
  for (int frame = 0; frame < 3; ++frame) {
    renderer.draw(document);
    frames.push_back(renderer.takeSnapshot());
    ASSERT_FALSE(frames.back().empty());
    if (frame > 0) {
      EXPECT_THAT(renderer.lastFrameTimings().counters.pathEncodes, testing::Eq(0u))
          << "Both scales stay cached, so frame " << frame << " must not re-encode.";
    }
  }

  // Only the identity-scale half is compared against the single-use render.
  const auto leftHalf = [](const RendererBitmap& bitmap) {
    return editor::tests::CropBitmap(bitmap, Vector2i(0, 0), Vector2i(64, 64));
  };
  editor::tests::CompareBitmapToBitmap(leftHalf(frames[0]), leftHalf(singleFrame),
                                       "fill_encode_two_scales_first_frame_vs_single",
                                       editor::tests::PixelmatchIdentityParams());
  for (size_t frame = 1; frame < frames.size(); ++frame) {
    editor::tests::CompareBitmapToBitmap(frames[frame], frames[0],
                                         "fill_encode_two_scales_frame_" + std::to_string(frame),
                                         editor::tests::PixelmatchIdentityParams());
  }
}

// More tolerance buckets of one element in a frame than it keeps encodes must not make them evict
// each other on every draw. The buckets cached first stay cached, the rest draw from a scratch
// encode, and every frame after the first renders the same.
TEST_F(RendererGeodeTest, FillEncodeAtThreeScalesDoesNotThrash) {
  ParseWarningSink warnings;
  auto parsed = parser::SVGParser::ParseSVG(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="192" height="64">
        <defs>
          <circle id="sprite" cx="8" cy="8" r="6" fill="#c040a0"/>
        </defs>
        <use href="#sprite"/>
        <use href="#sprite" transform="translate(32 0) scale(1.5)"/>
        <use href="#sprite" transform="translate(80 0) scale(3)"/>
      </svg>)svg",
      warnings);
  ASSERT_THAT(parsed.hasError(), testing::IsFalse());
  SVGDocument document = std::move(parsed).result();
  RendererGeode renderer = createRenderer();

  std::vector<RendererBitmap> frames;
  std::vector<uint64_t> bufferWriteBytes;
  for (int frame = 0; frame < 4; ++frame) {
    renderer.draw(document);
    frames.push_back(renderer.takeSnapshot());
    ASSERT_FALSE(frames.back().empty());
    const auto& counters = renderer.lastFrameTimings().counters;
    bufferWriteBytes.push_back(counters.bufferWriteBytes);
    if (frame > 0) {
      EXPECT_THAT(counters.pathEncodes, testing::Le(1u))
          << "Only the bucket without a cached encode may encode in warm frame " << frame << ".";
    }
  }
  for (size_t frame = 2; frame < frames.size(); ++frame) {
    EXPECT_THAT(bufferWriteBytes[frame], testing::Eq(bufferWriteBytes[1]))
        << "Warm frame " << frame << " must upload no more than the first warm frame.";
    editor::tests::CompareBitmapToBitmap(frames[frame], frames[1],
                                         "fill_encode_three_scales_frame_" + std::to_string(frame),
                                         editor::tests::PixelmatchIdentityParams());
  }
  editor::tests::CompareBitmapToBitmap(frames[1], frames[0], "fill_encode_three_scales_frame_1",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST_F(RendererGeodeTest, MidFrameOffscreenGeometryMutationPreservesRecordedOuterDraw) {
  ParseWarningSink warnings;
  auto parsed = parser::SVGParser::ParseSVG(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="32" height="16">
        <rect id="target" width="8" height="8" fill="lime"/>
      </svg>)svg",
      warnings);
  ASSERT_THAT(parsed.hasError(), testing::IsFalse());
  SVGDocument document = std::move(parsed).result();
  auto target = document.querySelector("#target");
  ASSERT_THAT(target, testing::Optional(testing::_));
  RendererGeode outer = createRenderer();
  outer.draw(document);
  const RendererBitmap baseline = outer.takeSnapshot();
  ASSERT_THAT(PixelAt(baseline, 4, 4), RgbaEq(0, 255, 0, 255));
  ASSERT_THAT(PixelAt(baseline, 12, 4), IsTransparent());

  RenderViewport viewport;
  viewport.size = Vector2d(32.0, 16.0);
  RendererGeode reference = createRenderer();
  reference.beginFrame(viewport);
  reference.setPaint(solidFill(css::RGBA(0, 255, 0, 255)));
  reference.drawRect(Box2d::FromXYWH(0, 0, 8, 8), StrokeParams{});
  reference.setPaint(solidFill(css::RGBA(255, 255, 255, 255)));
  reference.drawRect(Box2d::FromXYWH(30, 14, 1, 1), StrokeParams{});
  reference.endFrame();
  const RendererBitmap expectedOuter = reference.takeSnapshot();
  ASSERT_THAT(expectedOuter.empty(), testing::IsFalse());
  ASSERT_THAT(expectedOuter.dimensions, testing::Eq(Vector2i(32, 16)));

  reference.beginFrame(viewport);
  reference.setPaint(solidFill(css::RGBA(0, 255, 0, 255)));
  reference.drawRect(Box2d::FromXYWH(0, 0, 16, 8), StrokeParams{});
  reference.endFrame();
  const RendererBitmap expectedInner = reference.takeSnapshot();
  ASSERT_THAT(expectedInner.empty(), testing::IsFalse());
  ASSERT_THAT(expectedInner.dimensions, testing::Eq(Vector2i(32, 16)));

  outer.beginFrame(viewport);
  outer.setPaint(solidFill(css::RGBA(0, 255, 0, 255)));
  const Path original = PathBuilder().addRect(Box2d::FromXYWH(0, 0, 8, 8)).build();
  outer.drawPath(PathShape{.path = &original, .sourceEntity = target->unsafeEntityHandle()},
                 StrokeParams{});
  outer.setPaint(solidFill(css::RGBA(255, 255, 255, 255)));
  outer.drawRect(Box2d::FromXYWH(30, 14, 1, 1), StrokeParams{});

  // The outer commands already use the old resident range when the inner draw replaces it.
  target->setAttribute("width", "16");
  std::unique_ptr<RendererInterface> inner = outer.createOffscreenInstance();
  ASSERT_THAT(inner, testing::NotNull());
  inner->draw(document);
  const RendererBitmap innerBitmap = inner->takeSnapshot();
  outer.endFrame();
  const RendererBitmap actual = outer.takeSnapshot();

  ASSERT_THAT(actual.empty(), testing::IsFalse());
  ASSERT_THAT(actual.dimensions, testing::Eq(Vector2i(32, 16)));
  ASSERT_THAT(innerBitmap.empty(), testing::IsFalse());
  ASSERT_THAT(innerBitmap.dimensions, testing::Eq(Vector2i(32, 16)));
  editor::tests::CompareBitmapToBitmap(actual, expectedOuter,
                                       "offscreen_geometry_preserves_outer_draw",
                                       editor::tests::PixelmatchIdentityParams());
  editor::tests::CompareBitmapToBitmap(innerBitmap, expectedInner,
                                       "offscreen_geometry_uses_new_bounds",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST_F(RendererGeodeTest, CallScopedFillGeometrySurvivesSourceCacheReplacement) {
  RendererGeode reference = createRenderer();
  beginFrame(reference);
  reference.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  reference.drawRect(Box2d({8, 8}, {14, 16}), StrokeParams{});
  reference.endFrame();
  const RendererBitmap expected = reference.takeSnapshot();
  ASSERT_THAT(expected.empty(), testing::IsFalse());
  ASSERT_THAT(expected.dimensions, testing::Eq(Vector2i(64, 64)));

  ParseWarningSink warnings;
  auto parsed = parser::SVGParser::ParseSVG(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
        <rect id="target" x="8" y="8" width="6" height="8" fill="red"/>
      </svg>)svg",
      warnings);
  ASSERT_THAT(parsed.hasError(), testing::IsFalse());
  SVGDocument document = std::move(parsed).result();
  auto target = document.querySelector("#target");
  ASSERT_THAT(target, testing::Optional(testing::_));
  RendererGeode renderer = createRenderer();
  renderer.draw(document);
  editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), expected,
                                       "call_scoped_fill_baseline",
                                       editor::tests::PixelmatchIdentityParams());

  Path borrowed = PathBuilder().addRect(Box2d({8, 8}, {14, 16})).build();
  beginFrame(renderer);
  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  renderer.drawPath(PathShape{.path = &borrowed,
                              .sourceEntity = target->unsafeEntityHandle(),
                              .allowDeferredGeometry = false},
                    StrokeParams{});
  // Recompute the source cache without a backend state change that could flush deferred draws.
  borrowed = Path();
  target->setAttribute("width", "12");
  RendererUtils::prepareDocumentForRendering(document, false, warnings);
  renderer.endFrame();
  const RendererBitmap actual = renderer.takeSnapshot();
  ASSERT_THAT(actual.empty(), testing::IsFalse());
  ASSERT_THAT(actual.dimensions, testing::Eq(Vector2i(64, 64)));
  editor::tests::CompareBitmapToBitmap(actual, expected, "call_scoped_fill_geometry",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST_F(RendererGeodeTest, CallScopedStrokeGeometrySurvivesSourceCacheReplacement) {
  StrokeParams stroke;
  stroke.strokeWidth = 2.0;
  RendererGeode reference = createRenderer();
  beginFrame(reference);
  reference.setPaint(solidStroke(css::RGBA(255, 0, 0, 255)));
  reference.drawRect(Box2d({8, 8}, {14, 16}), stroke);
  reference.endFrame();
  const RendererBitmap expected = reference.takeSnapshot();
  ASSERT_THAT(expected.empty(), testing::IsFalse());
  ASSERT_THAT(expected.dimensions, testing::Eq(Vector2i(64, 64)));

  ParseWarningSink warnings;
  auto parsed = parser::SVGParser::ParseSVG(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
        <rect id="target" x="8" y="8" width="6" height="8" fill="none" stroke="red"
              stroke-width="2"/>
      </svg>)svg",
      warnings);
  ASSERT_THAT(parsed.hasError(), testing::IsFalse());
  SVGDocument document = std::move(parsed).result();
  auto target = document.querySelector("#target");
  ASSERT_THAT(target, testing::Optional(testing::_));
  RendererGeode renderer = createRenderer();
  renderer.draw(document);
  editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), expected,
                                       "call_scoped_stroke_baseline",
                                       editor::tests::PixelmatchIdentityParams());

  Path borrowed = PathBuilder().addRect(Box2d({8, 8}, {14, 16})).build();
  beginFrame(renderer);
  renderer.setPaint(solidStroke(css::RGBA(255, 0, 0, 255)));
  renderer.drawPath(PathShape{.path = &borrowed,
                              .sourceEntity = target->unsafeEntityHandle(),
                              .allowDeferredGeometry = false},
                    stroke);
  // Recompute the source cache without a backend state change that could flush deferred draws.
  borrowed = Path();
  target->setAttribute("width", "12");
  RendererUtils::prepareDocumentForRendering(document, false, warnings);
  renderer.endFrame();
  const RendererBitmap actual = renderer.takeSnapshot();
  ASSERT_THAT(actual.empty(), testing::IsFalse());
  ASSERT_THAT(actual.dimensions, testing::Eq(Vector2i(64, 64)));
  editor::tests::CompareBitmapToBitmap(actual, expected, "call_scoped_stroke_geometry",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST_F(RendererGeodeTest, CubicFillRefinementPreservesPendingInstances) {
  Registry registry;
  const EntityHandle source(registry, registry.create());
  const Path cubic = PathBuilder()
                         .moveTo({0.0, 0.0})
                         .curveTo({1.0 / 3.0, 0.0}, {2.0 / 3.0, 0.0}, {1.0, 1.0})
                         .lineTo({1.0, 0.0})
                         .closePath()
                         .build();
  const auto render = [&](EntityHandle entity) {
    RendererGeode renderer = createRenderer();
    beginFrame(renderer);
    renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
    renderer.drawPath(PathShape{.path = &cubic, .sourceEntity = entity}, StrokeParams{});
    renderer.setTransform(Transform2d::Scale(32.0) * Transform2d::Translate(16.0, 16.0));
    renderer.drawPath(PathShape{.path = &cubic, .sourceEntity = entity}, StrokeParams{});
    renderer.endFrame();
    return renderer.takeSnapshot();
  };
  editor::tests::CompareBitmapToBitmap(render(source), render(EntityHandle()),
                                       "cubic_fill_scale_batch",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST_F(RendererGeodeTest, CubicFillRefinementReuploadsSolidAndGradientResidency) {
  for (bool gradient : {false, true}) {
    SCOPED_TRACE(gradient);
    ParseWarningSink warningSink;
    constexpr const char* source = R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
        <defs><linearGradient id="g"><stop stop-color="red"/>
          <stop offset="1" stop-color="blue"/></linearGradient></defs>
        <path id="p" d="M0 0 C.3333333333333333 0 .6666666666666666 0 1 1 L1 0 Z"/>
      </svg>)svg";
    const auto makeDocument = [&]() {
      auto parsed = parser::SVGParser::ParseSVG(source, warningSink);
      EXPECT_THAT(parsed.hasError(), testing::IsFalse());
      SVGDocument document = std::move(parsed).result();
      document.querySelector("#p")->setAttribute("fill", gradient ? "url(#g)" : "red");
      RendererUtils::prepareDocumentForRendering(document, false, warningSink);
      return document;
    };
    SVGDocument warmedDocument = makeDocument();
    SVGDocument freshDocument = makeDocument();
    const auto render = [&](RendererGeode& renderer, SVGDocument& document, double scale) {
      const Entity entity = document.querySelector("#p")->unsafeEntityHandle().entity();
      RenderViewport viewport;
      viewport.size = Vector2d(64.0, 64.0);
      viewport.devicePixelRatio = 1.0;
      RendererDriver(renderer).drawEntityRange(document.registry(), entity, entity, viewport,
                                               Transform2d::Scale(scale));
      return renderer.takeSnapshot();
    };
    RendererGeode warmed = createRenderer();
    RendererGeode fresh = createRenderer();
    (void)render(warmed, warmedDocument, 1.0);
    const RendererBitmap actual = render(warmed, warmedDocument, 32.0);
    const RendererBitmap expected = render(fresh, freshDocument, 32.0);
    editor::tests::CompareBitmapToBitmap(actual, expected,
                                         gradient ? "cubic_gradient_zoom" : "cubic_solid_zoom",
                                         editor::tests::PixelmatchIdentityParams());
  }
}

/// The resident gradient draw skips its uniform rewrite when the
/// GradientUniforms block is byte-identical to the previous frame, so every
/// gradient property the shader consumes must flow through that block for
/// the skip to be sound. Pin the end-to-end behavior with pixels: an
/// unchanged document renders identically through the steady-state resident
/// path, and a stop-color edit (no geometry change, so only the
/// uniform-comparison path can carry it) must change the next frame.
TEST_F(RendererGeodeTest, GradientStopEditInvalidatesResidentGradientDraw) {
  ParseWarningSink warningSink;
  auto maybeDocument = parser::SVGParser::ParseSVG(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
             <defs>
               <linearGradient id="g">
                 <stop id="s0" offset="0" stop-color="#ff0000"/>
                 <stop id="s1" offset="1" stop-color="#ff0000"/>
               </linearGradient>
             </defs>
             <rect width="64" height="64" fill="url(#g)"/>
           </svg>)svg",
      warningSink);
  ASSERT_FALSE(maybeDocument.hasError()) << maybeDocument.error();
  SVGDocument document = std::move(maybeDocument).result();

  RendererGeode renderer = createRenderer();
  renderer.draw(document);
  const RendererBitmap first = renderer.takeSnapshot();
  ASSERT_FALSE(first.empty());
  EXPECT_THAT(PixelAt(first, 32, 32), RgbaEq(255, 0, 0, 255));

  // Unchanged second frame: the resident path skips the uniform write and
  // must render identically.
  renderer.draw(document);
  const RendererBitmap second = renderer.takeSnapshot();
  ASSERT_FALSE(second.empty());
  EXPECT_THAT(PixelAt(second, 32, 32), RgbaEq(255, 0, 0, 255));

  auto stop0 = document.querySelector("#s0");
  auto stop1 = document.querySelector("#s1");
  ASSERT_TRUE(stop0.has_value());
  ASSERT_TRUE(stop1.has_value());
  stop0->setAttribute("stop-color", "#00ff00");
  stop1->setAttribute("stop-color", "#00ff00");

  renderer.draw(document);
  const RendererBitmap third = renderer.takeSnapshot();
  ASSERT_FALSE(third.empty());
  EXPECT_THAT(PixelAt(third, 32, 32), RgbaEq(0, 255, 0, 255))
      << "A gradient stop edit must invalidate the resident gradient draw's cached uniforms";
}

/// The stroke encode is rebuilt IN PLACE on a stroke-param change (same
/// storage address, no component removal), so gradient-stroke residence
/// invalidation rests on the explicit reset at the rebuild site. Pin it
/// with pixels: a stroke-width edit with no geometry change must change
/// the next frame, and an unchanged document must render identically
/// through the steady-state resident path.
TEST_F(RendererGeodeTest, StrokeWidthEditInvalidatesResidentGradientStroke) {
  ParseWarningSink warningSink;
  auto maybeDocument = parser::SVGParser::ParseSVG(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
             <defs>
               <linearGradient id="g" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="64"
                                y2="0">
                 <stop offset="0" stop-color="#00ff00"/>
                 <stop offset="1" stop-color="#00ff00"/>
               </linearGradient>
             </defs>
             <path id="p" d="M 8 32 L 56 32" fill="none" stroke="url(#g)" stroke-width="4"/>
           </svg>)svg",
      warningSink);
  ASSERT_FALSE(maybeDocument.hasError()) << maybeDocument.error();
  SVGDocument document = std::move(maybeDocument).result();

  RendererGeode renderer = createRenderer();
  renderer.draw(document);
  const RendererBitmap first = renderer.takeSnapshot();
  ASSERT_FALSE(first.empty());
  // 12px above the line center: outside a 4px-wide stroke, inside a 28px one.
  EXPECT_THAT(PixelAt(first, 32, 20), IsTransparent());
  EXPECT_THAT(PixelAt(first, 32, 32), RgbaEq(0, 255, 0, 255));

  // Unchanged second frame rides the resident path and must be identical.
  renderer.draw(document);
  const RendererBitmap second = renderer.takeSnapshot();
  ASSERT_FALSE(second.empty());
  EXPECT_THAT(PixelAt(second, 32, 20), IsTransparent());
  EXPECT_THAT(PixelAt(second, 32, 32), RgbaEq(0, 255, 0, 255));

  auto path = document.querySelector("#p");
  ASSERT_TRUE(path.has_value());
  path->setAttribute("stroke-width", "28");

  renderer.draw(document);
  const RendererBitmap third = renderer.takeSnapshot();
  ASSERT_FALSE(third.empty());
  EXPECT_THAT(PixelAt(third, 32, 20), RgbaEq(0, 255, 0, 255))
      << "A stroke-width edit must invalidate the resident gradient-stroke draw";
}

/// Removing one entity's residence component makes entt swap-and-pop the
/// component pool: the LAST entity's component is move-assigned over the
/// removed slot. The move must carry the record slot and its slab, or the
/// removed component's destructor frees the survivor's record while the
/// survivor's cached bind group still binds it; once that record storage is
/// reused by another entity, the survivor renders with that entity's paint.
/// Pin it with pixels across the frames where the freed slot gets reused.
TEST_F(RendererGeodeTest, GeometryEditKeepsSiblingResidentPaintsIntact) {
  ParseWarningSink warningSink;
  auto maybeDocument = parser::SVGParser::ParseSVG(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="96" height="32">
             <path id="a" d="M 2 2 L 30 2 L 30 30 L 2 30 Z" fill="#ff0000"/>
             <path id="b" d="M 34 2 L 62 2 L 62 30 L 34 30 Z" fill="#00ff00"/>
             <path id="c" d="M 66 2 L 94 2 L 94 30 L 66 30 Z" fill="#0000ff"/>
           </svg>)svg",
      warningSink);
  ASSERT_FALSE(maybeDocument.hasError()) << maybeDocument.error();
  SVGDocument document = std::move(maybeDocument).result();

  RendererGeode renderer = createRenderer();
  renderer.draw(document);
  const RendererBitmap first = renderer.takeSnapshot();
  ASSERT_FALSE(first.empty());
  EXPECT_THAT(PixelAt(first, 16, 16), RgbaEq(255, 0, 0, 255));
  EXPECT_THAT(PixelAt(first, 48, 16), RgbaEq(0, 255, 0, 255));
  EXPECT_THAT(PixelAt(first, 80, 16), RgbaEq(0, 0, 255, 255));

  // Edit the FIRST entity's geometry: the residence listener removes its
  // component mid-pool, which swap-and-pops the last entity's component
  // over it.
  auto pathA = document.querySelector("#a");
  ASSERT_TRUE(pathA.has_value());
  pathA->setAttribute("d", "M 2 6 L 30 6 L 30 26 L 2 26 Z");

  // Frame 2 performs the swap-and-pop (the listener fires inside the
  // draw) and frees the removed component's record slot; frame 3 merges
  // that slot into the free list. A second edit then forces a fresh
  // record allocation at frame 4, which reuses the freed slot: if the
  // survivor's bind group still points there, the survivor renders the
  // edited entity's paint.
  renderer.draw(document);
  renderer.draw(document);
  // Edit BOTH untouched-geometry siblings' shared shape class: two fresh
  // record allocations pop the free list past the edited entity's own
  // returned index and onto the slot the swap-and-pop shell freed, which
  // is where a survivor with a stale bind group would pick up another
  // entity's record.
  pathA->setAttribute("d", "M 2 4 L 30 4 L 30 28 L 2 28 Z");
  auto pathB = document.querySelector("#b");
  ASSERT_TRUE(pathB.has_value());
  pathB->setAttribute("d", "M 34 4 L 62 4 L 62 28 L 34 28 Z");
  renderer.draw(document);
  renderer.draw(document);
  const RendererBitmap after = renderer.takeSnapshot();
  ASSERT_FALSE(after.empty());
  EXPECT_THAT(PixelAt(after, 16, 16), RgbaEq(255, 0, 0, 255))
      << "The edited entity must render its own paint at the new geometry";
  EXPECT_THAT(PixelAt(after, 48, 16), RgbaEq(0, 255, 0, 255))
      << "An untouched sibling must keep its own paint after the pool swap";
  EXPECT_THAT(PixelAt(after, 80, 16), RgbaEq(0, 0, 255, 255))
      << "The swap-and-pop survivor must keep its own record and paint";
}

TEST_F(RendererGeodeTest, SharedPhysicalOwnerDrawPathExportsTextureSnapshot) {
  std::shared_ptr<geode::GeodeDevice> host = geode::GeodeDevice::CreateHeadless();
  ASSERT_TRUE(host != nullptr);

  auto embeddedUnique = geode::GeodeDevice::CreateOverPhysicalDeviceOwner(
      host->physicalDeviceOwner(), host->textureFormat());
  ASSERT_NE(embeddedUnique, nullptr);

  std::shared_ptr<geode::GeodeDevice> embedded(std::move(embeddedUnique));
  const geode::GeodeGpuContext& gpuContext = embedded->gpuContext();
  ASSERT_NE(gpuContext.dummyPatternTextureView, nullptr);
  ASSERT_NE(gpuContext.dummyPatternSampler, nullptr);
  ASSERT_NE(gpuContext.dummyClipMaskTextureView, nullptr);
  ASSERT_NE(gpuContext.identityInstanceRecordBuffer, nullptr);
  ASSERT_TRUE(gpuContext.dummyPatternTextureView->isValid());
  ASSERT_TRUE(gpuContext.dummyPatternSampler->isValid());
  ASSERT_TRUE(gpuContext.dummyClipMaskTextureView->isValid());
  ASSERT_TRUE(gpuContext.identityInstanceRecordBuffer->isValid());

  RendererGeode renderer(embedded);
  beginFrame(renderer);
  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  renderer.drawRect(Box2d({8, 8}, {56, 56}), StrokeParams{});
  renderer.endFrame();

  std::shared_ptr<const RendererTextureSnapshot> texture = renderer.takeTextureSnapshot();
  ASSERT_TRUE(texture != nullptr);
  EXPECT_EQ(texture->backend(), RendererTextureSnapshotBackend::Geode);
  EXPECT_EQ(texture->dimensions(),
            Vector2i(static_cast<int>(kViewportSize), static_cast<int>(kViewportSize)));
}

TEST_F(RendererGeodeTest, BgraTargetSnapshotReturnsStraightRgba) {
  std::shared_ptr<geode::GeodeDevice> bgraContext =
      geode::GeodeDevice::CreateHeadless(gpu::TextureFormat::BGRA8Unorm);
  ASSERT_NE(bgraContext, nullptr);
  expectBgraTargetSnapshotIsStraightRgba(bgraContext);
}

/// A BGRA target rendered by a second logical context over the same native root reads back the
/// same way as the selecting context's target.
TEST_F(RendererGeodeTest, SharedBgraTargetSnapshotReturnsStraightRgba) {
  std::shared_ptr<geode::GeodeDevice> host =
      geode::GeodeDevice::CreateHeadless(gpu::TextureFormat::BGRA8Unorm);
  ASSERT_NE(host, nullptr);

  std::shared_ptr<geode::GeodeDevice> embedded = geode::GeodeDevice::CreateOverPhysicalDeviceOwner(
      host->physicalDeviceOwner(), gpu::TextureFormat::BGRA8Unorm);
  ASSERT_NE(embedded, nullptr);
  expectBgraTargetSnapshotIsStraightRgba(embedded);
}

/// Filling a path with a solid red paint should produce red pixels at the
/// path's interior.
TEST_F(RendererGeodeTest, DrawPathWithSolidFill) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  renderer.setTransform(Transform2d());

  const Path path = PathBuilder().addRect(Box2d({16, 16}, {48, 48})).build();
  PathShape shape;
  shape.path = &path;
  shape.fillRule = FillRule::NonZero;
  renderer.drawPath(shape, StrokeParams{});

  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  ASSERT_FALSE(snap.empty());

  // Center should be red, transparent at the corner.
  auto center = PixelAt(snap, 32, 32);
  EXPECT_THAT(center, RgbaEq(255, 0, 0, 255));

  auto corner = PixelAt(snap, 4, 4);
  EXPECT_THAT(corner, IsTransparent()) << "Corner should be transparent";
}

/// `drawRect` is a convenience over `drawPath`. Verify it produces the same
/// pixels.
TEST_F(RendererGeodeTest, DrawRectGreenFill) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  renderer.setPaint(solidFill(css::RGBA(0, 255, 0, 255)));
  renderer.drawRect(Box2d({8, 8}, {56, 56}), StrokeParams{});

  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  auto center = PixelAt(snap, 32, 32);
  EXPECT_THAT(center, RgbaEq(0, 255, 0, 255));
}

/// `drawEllipse` should fill an elliptical area. Center inside, far corners
/// outside.
TEST_F(RendererGeodeTest, DrawEllipseBlueFill) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  renderer.setPaint(solidFill(css::RGBA(0, 0, 255, 255)));
  renderer.drawEllipse(Box2d({12, 12}, {52, 52}), StrokeParams{});

  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  auto center = PixelAt(snap, 32, 32);
  EXPECT_THAT(center, RgbaEq(0, 0, 255, 255));

  // Far corner: outside the inscribed ellipse.
  auto corner = PixelAt(snap, 2, 2);
  EXPECT_THAT(corner, IsTransparent()) << "Corner should be transparent";
}

/// Transform stack should compose like the other backends. Apply a translate
/// via push, draw, pop, draw - verify both shapes land where expected.
TEST_F(RendererGeodeTest, PushPopTransform) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));

  // Draw a small rect at (8,8)-(16,16) in path space, but with a translate
  // pushed so it lands at (24,24)-(32,32) in pixel space.
  renderer.pushTransform(Transform2d::Translate(Vector2d(16, 16)));
  const Path path = PathBuilder().addRect(Box2d({8, 8}, {16, 16})).build();
  PathShape shape;
  shape.path = &path;
  shape.fillRule = FillRule::NonZero;
  renderer.drawPath(shape, StrokeParams{});
  renderer.popTransform();

  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  auto inside = PixelAt(snap, 28, 28);
  EXPECT_THAT(inside, Rgba(testing::Eq(255), testing::_, testing::_, testing::_))
      << "Translated rect should cover (28, 28)";

  auto outside = PixelAt(snap, 12, 12);
  EXPECT_THAT(outside, IsTransparent()) << "Original rect position should be empty";
}

/// Stroke a rectangle outline with no fill: the stroke band should be the
/// stroke color and the interior / exterior should be transparent.
TEST_F(RendererGeodeTest, StrokeRectOutline) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  renderer.setPaint(solidStroke(css::RGBA(255, 0, 0, 255)));
  StrokeParams stroke;
  stroke.strokeWidth = 4.0;
  stroke.lineCap = StrokeLinecap::Butt;
  stroke.lineJoin = StrokeLinejoin::Miter;

  const Path path = PathBuilder().addRect(Box2d({16, 16}, {48, 48})).build();
  PathShape shape;
  shape.path = &path;
  shape.fillRule = FillRule::NonZero;
  renderer.drawPath(shape, stroke);

  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();

  // On the top edge (y=16), well inside the horizontal extent, the stroke
  // should contribute red. The stroke extends outward by width/2 = 2, so
  // any pixel with y in [14, 18) and x in [14, 50) should be touched.
  auto top = PixelAt(snap, 32, 16);
  EXPECT_THAT(top, Rgba(testing::Eq(255), testing::Eq(0), testing::Eq(0), testing::_));

  // The interior of the rect (center) should be transparent - fill=none.
  auto center = PixelAt(snap, 32, 32);
  EXPECT_THAT(center, IsTransparent()) << "Interior should be transparent (fill=none)";

  // Far corner: outside everything.
  auto corner = PixelAt(snap, 2, 2);
  EXPECT_THAT(corner, IsTransparent()) << "Corner should be transparent";
}

TEST_F(RendererGeodeTest, AffineStrokePiecesMatchExplicitUnion) {
  const Path centerline = PathBuilder()
                              .moveTo({30, 10})
                              .lineTo({34, 10})
                              .lineTo({34, 24})
                              .lineTo({46, 24})
                              .lineTo({46, 28})
                              .lineTo({34, 28})
                              .lineTo({34, 55})
                              .lineTo({30, 55})
                              .lineTo({30, 28})
                              .lineTo({22, 28})
                              .lineTo({22, 24})
                              .lineTo({30, 24})
                              .closePath()
                              .build();
  const Path pieces = centerline.strokeToFill({.width = 6.0}, 0.1);
  const Path boundary = PathBuilder()
                            .moveTo({27, 7})
                            .lineTo({37, 7})
                            .lineTo({37, 21})
                            .lineTo({49, 21})
                            .lineTo({49, 31})
                            .lineTo({37, 31})
                            .lineTo({37, 58})
                            .lineTo({27, 58})
                            .lineTo({27, 31})
                            .lineTo({19, 31})
                            .lineTo({19, 21})
                            .lineTo({27, 21})
                            .closePath()
                            .build();
  const std::array<Transform2d, 2> canvasFromPath = {
      Transform2d::Rotate(0.31) * Transform2d::Translate({24.25, 10.25}),
      Transform2d::SkewX(0.27) * Transform2d::SkewY(-0.19) *
          Transform2d::Translate({10.25, 20.25})};
  for (size_t index = 0; index < canvasFromPath.size(); ++index) {
    // Bake the transform so the encoded curves themselves contain diagonal coincident edges.
    const Path actualPath = TransformPath(pieces, canvasFromPath[index]);
    const Path expectedPath = TransformPath(boundary, canvasFromPath[index]);
    for (uint8_t alpha : {uint8_t{255}, uint8_t{128}}) {
      const auto render = [&](const Path& path) {
        RendererGeode renderer = createRenderer();
        RenderViewport viewport;
        viewport.size = Vector2d(128, 128);
        viewport.devicePixelRatio = 1.0;
        renderer.beginFrame(viewport);
        renderer.setPaint(solidFill(css::RGBA(255, 0, 255, alpha)));
        PathShape shape;
        shape.path = &path;
        shape.fillRule = FillRule::NonZero;
        renderer.drawPath(shape, StrokeParams{});
        renderer.endFrame();
        return renderer.takeSnapshot();
      };
      editor::tests::CompareBitmapToBitmap(
          render(actualPath), render(expectedPath),
          "affine_stroke_union_" + std::to_string(index) + "_" + std::to_string(alpha),
          editor::tests::PixelmatchIdentityParams());
    }
  }
}

TEST_F(RendererGeodeTest, StrokeSharedVertexDoesNotExtendVertically) {
  RendererGeode renderer = createRenderer();
  RenderViewport viewport;
  viewport.size = Vector2d(200, 200);
  viewport.devicePixelRatio = 1.0;
  renderer.beginFrame(viewport);

  renderer.setPaint(solidStroke(css::RGBA(0, 0, 255, 255)));
  StrokeParams stroke;
  stroke.strokeWidth = 1.0;
  stroke.lineJoin = StrokeLinejoin::Miter;

  const Path path = PathBuilder()
                        .moveTo({50, 85})
                        .lineTo({65, 135})
                        .lineTo({150, 135})
                        .lineTo({150, 85})
                        .quadTo({100, 45}, {50, 85})
                        .closePath()
                        .build();
  PathShape shape;
  shape.path = &path;
  shape.fillRule = FillRule::NonZero;
  renderer.drawPath(shape, stroke);
  renderer.endFrame();

  const RendererBitmap snap = renderer.takeSnapshot();
  ASSERT_FALSE(snap.empty());
  for (int y = 90; y <= 120; y += 10) {
    EXPECT_THAT(PixelAt(snap, 65, y), IsTransparent())
        << "Stroke must not extend vertically above the shared vertex at y=" << y;
  }
}
/// Dash patterns beyond `Path::strokeToFill`'s allocation guardrail fall back to a solid stroke.
/// The closed-path outline must retain EvenOdd semantics so its interior remains hollow.
TEST_F(RendererGeodeTest, OversizedDashArrayPreservesSolidStrokeFallback) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  renderer.setPaint(solidStroke(css::RGBA(255, 0, 0, 255)));
  StrokeParams stroke;
  stroke.strokeWidth = 4.0;
  stroke.dashArray.assign(257, 1.0);

  const Path path = PathBuilder().addRect(Box2d({16, 16}, {48, 48})).build();
  PathShape shape;
  shape.path = &path;
  shape.fillRule = FillRule::NonZero;
  renderer.drawPath(shape, stroke);

  renderer.endFrame();

  const RendererBitmap snapshot = renderer.takeSnapshot();
  ASSERT_FALSE(snapshot.empty());
  EXPECT_THAT(PixelAt(snapshot, 32, 16), RgbaEq(255, 0, 0, 255))
      << "oversized dash fallback should retain the solid stroke band";
  EXPECT_THAT(PixelAt(snapshot, 32, 32), IsTransparent())
      << "oversized dash fallback must not fill the closed stroke interior";
}

// ----------------------------------------------------------------------------
// Device-aware stroke flattening.
//
// Stroke outlines are generated by `Path::strokeToFill`, which flattens curves
// to line segments using a tolerance expressed in the PATH's coordinate space.
// Geometry is submitted in document space and scaled by the device transform on
// the GPU, so a fixed path-local tolerance is magnified by the view scale: a
// circle (four kappa cubics) flattened once at document scale shows as a
// visible chain of segments as soon as the view is zoomed in. That is a
// rendering-correctness violation, so the tolerance must be derived from the
// draw-time transform at every stroke call site.
//
// `strokeOutlinePoints` counts the points of the stroke outlines a frame
// actually rebuilt, which makes the derivation observable: finer flattening at
// a higher device scale means strictly more outline points. A scale-blind
// pipeline reports the same count at 1x and at 32x.
// ----------------------------------------------------------------------------

/// The editor-overlay shape of the bug: a document-space path drawn with no
/// source entity under a zoom transform (`OverlayRenderer` submits selection
/// chrome exactly this way). The stroke outline must be re-flattened for the
/// device scale rather than reused at document density.
TEST_F(RendererGeodeTest, StrokeFlatteningTracksDeviceScaleWithoutASourceEntity) {
  RendererGeode renderer = createRenderer();

  const Path path = PathBuilder().addCircle(Vector2d(32.0, 32.0), 12.0).build();
  PathShape shape;
  shape.path = &path;
  shape.fillRule = FillRule::NonZero;

  StrokeParams stroke;
  stroke.strokeWidth = 2.0;

  const auto outlinePointsAtScale = [&](double scale) {
    beginFrame(renderer);
    renderer.setPaint(solidStroke(css::RGBA(255, 0, 0, 255)));
    renderer.setTransform(Transform2d::Scale(scale));
    renderer.drawPath(shape, stroke);
    renderer.endFrame();
    return renderer.lastFrameTimings().counters.strokeOutlinePoints;
  };

  const uint64_t atOne = outlinePointsAtScale(1.0);
  const uint64_t atThirtyTwo = outlinePointsAtScale(32.0);

  ASSERT_GT(atOne, 0u) << "The stroke outline counter must observe the 1:1 draw.";
  EXPECT_GT(atThirtyTwo, atOne * 4u)
      << "A circle stroked at 32x device scale must be flattened far more finely than the same "
         "circle at 1:1 ("
      << atThirtyTwo << " vs " << atOne
      << " outline points). Equal counts mean the flattening tolerance ignored the device "
         "transform, which is what renders zoomed-in curves as visible polygons.";
}

/// Same invariant through the per-entity stroke cache. That cache is keyed by
/// `StrokeStyle`, which does not change with zoom, so the derived flattening
/// tolerance has to be part of the key: otherwise a zoomed-in frame serves the
/// coarse outline that was tessellated for the previous scale.
TEST_F(RendererGeodeTest, StrokeCacheRebuildsOutlineWhenTheDeviceScaleChanges) {
  ParseWarningSink warningSink;
  auto maybeDocument = parser::SVGParser::ParseSVG(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
             <circle id="c" cx="50" cy="50" r="20"
                     style="fill: none; stroke: black; stroke-width: 2"/>
           </svg>)svg",
      warningSink);
  ASSERT_FALSE(maybeDocument.hasError()) << maybeDocument.error();
  SVGDocument document = std::move(maybeDocument).result();
  auto circle = document.querySelector("#c");
  ASSERT_TRUE(circle.has_value());
  const Entity circleEntity = circle->unsafeEntityHandle().entity();

  RendererUtils::prepareDocumentForRendering(document, /*verbose=*/false, warningSink);

  RendererGeode renderer = createRenderer();
  const auto outlinePointsAtScale = [&](double scale) {
    RenderViewport viewport;
    viewport.size = Vector2d(100.0 * scale, 100.0 * scale);
    viewport.devicePixelRatio = 1.0;
    RendererDriver driver(renderer);
    driver.drawEntityRange(document.registry(), circleEntity, circleEntity, viewport,
                           Transform2d::Scale(scale));
    return renderer.lastFrameTimings().counters.strokeOutlinePoints;
  };

  const uint64_t atOne = outlinePointsAtScale(1.0);
  const uint64_t atThirtyTwo = outlinePointsAtScale(32.0);

  ASSERT_GT(atOne, 0u) << "The first draw must build (and count) the stroke outline.";
  EXPECT_GT(atThirtyTwo, atOne * 4u)
      << "Zooming to 32x must re-derive the stroke outline (" << atThirtyTwo << " vs " << atOne
      << " outline points). A zero or unchanged count means the stroke cache served the outline "
         "flattened for the previous device scale.";
}

/// The flip side of the cache-key change: the derived tolerance is quantized to
/// power-of-two scale buckets, so a continuous zoom inside one bucket must not
/// re-flatten anything. Without the quantization every zoom frame would rebuild
/// and re-encode every stroked path.
TEST_F(RendererGeodeTest, StrokeCacheIsReusedWithinAScaleBucket) {
  ParseWarningSink warningSink;
  auto maybeDocument = parser::SVGParser::ParseSVG(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
             <circle id="c" cx="50" cy="50" r="20"
                     style="fill: none; stroke: black; stroke-width: 2"/>
           </svg>)svg",
      warningSink);
  ASSERT_FALSE(maybeDocument.hasError()) << maybeDocument.error();
  SVGDocument document = std::move(maybeDocument).result();
  auto circle = document.querySelector("#c");
  ASSERT_TRUE(circle.has_value());
  const Entity circleEntity = circle->unsafeEntityHandle().entity();

  RendererUtils::prepareDocumentForRendering(document, /*verbose=*/false, warningSink);

  RendererGeode renderer = createRenderer();
  const auto flattensAtScale = [&](double scale) {
    RenderViewport viewport;
    viewport.size = Vector2d(400.0, 400.0);
    viewport.devicePixelRatio = 1.0;
    RendererDriver driver(renderer);
    driver.drawEntityRange(document.registry(), circleEntity, circleEntity, viewport,
                           Transform2d::Scale(scale));
    return renderer.lastFrameTimings().counters.strokeOutlineFlattens;
  };

  ASSERT_GT(flattensAtScale(2.5), 0u) << "The first draw at this scale must build the outline.";
  EXPECT_EQ(flattensAtScale(2.9), 0u)
      << "A zoom step inside the same power-of-two scale bucket must reuse the cached stroke "
         "outline instead of re-flattening every frame.";
}

/// One edit to a document whose source path is stroked through three `<use>` instances with
/// different stroke styles, so the source path's stroke cache holds several outlines when the
/// edit lands. `target` names the edited element: the instance `edited`, the source path `p`, or
/// the group `zoom` that scales every instance.
struct StrokeKeyEdit {
  std::string_view name;       //!< Case name, also the comparison's artifact name.
  std::string_view target;     //!< Id of the edited element.
  std::string_view attribute;  //!< Attribute the edit writes.
  std::string_view before;     //!< Attribute value before the edit.
  std::string_view after;      //!< Attribute value after the edit.
  std::string_view extra;      //!< Further attributes on the edited instance.
};

/// Prints the case name, so a failure names the edit. @param edit Case. @param os Stream.
void PrintTo(const StrokeKeyEdit& edit, std::ostream* os) {
  *os << edit.name;
}

/// The shared-source document with `edit.attribute` of `edit.target` set to \p value.
std::string SharedStrokeSourceSvg(const StrokeKeyEdit& edit, std::string_view value) {
  const auto valueFor = [&](std::string_view target, std::string_view attribute,
                            std::string_view fallback) {
    return std::string(edit.target == target && edit.attribute == attribute ? value : fallback);
  };
  std::string editedAttribute;
  if (edit.target == "edited") {
    editedAttribute = std::string(edit.attribute) + "=\"" + std::string(value) + "\"";
  }
  return R"svg(<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" )svg"
         R"svg(width="400" height="240"><defs><path id="p" fill="none" d=")svg" +
         valueFor("p", "d", "M10,40 L30,10 L50,40 C60,55 75,55 85,40") + R"svg(" pathLength=")svg" +
         valueFor("p", "pathLength", "100") + R"svg("/></defs><g id="zoom" transform=")svg" +
         valueFor("zoom", "transform", "scale(1)") +
         R"svg("><use xlink:href="#p" stroke="#000" stroke-width="3"/>)svg"
         R"svg(<use xlink:href="#p" x="90" stroke="#00c" stroke-width="2" stroke-linejoin="round" )svg"
         R"svg(stroke-dasharray="5,2"/><use id="edited" xlink:href="#p" y="50" stroke="#c00" )svg" +
         std::string(edit.extra) + " " + editedAttribute + "/></g></svg>";
}

class RendererGeodeStrokeKeyTest : public RendererGeodeTest,
                                   public ::testing::WithParamInterface<StrokeKeyEdit> {};

/// Every input of a cached stroke outline is part of its key: after an edit to any of them, the
/// next frame draws what a fresh document with the edited value draws, never the outline cached
/// for the old value.
TEST_P(RendererGeodeStrokeKeyTest, EditRendersLikeAFreshDocument) {
  const StrokeKeyEdit& edit = GetParam();
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  const auto parse = [&](std::string_view value) {
    auto parsed = parser::SVGParser::ParseSVG(SharedStrokeSourceSvg(edit, value), warnings);
    EXPECT_THAT(parsed.hasError(), testing::IsFalse());
    return std::move(parsed).result();
  };

  SVGDocument document = parse(edit.before);
  RendererGeode renderer = createRenderer();
  renderer.draw(document);
  renderer.draw(document);
  const RendererBitmap beforeEdit = renderer.takeSnapshot();
  ASSERT_THAT(beforeEdit.empty(), testing::IsFalse());

  auto target = document.querySelector("#" + std::string(edit.target));
  ASSERT_THAT(target, testing::Optional(testing::_));
  if (edit.attribute == "pathLength") {
    // Written through the DOM setter: a runtime `setAttribute("pathLength", ...)` does not
    // reach the element's path length (#2609).
    target->cast<SVGGeometryElement>().setPathLength(std::stod(std::string(edit.after)));
  } else {
    target->setAttribute(edit.attribute, edit.after);
  }
  renderer.draw(document);
  const RendererBitmap afterEdit = renderer.takeSnapshot();

  SVGDocument fresh = parse(edit.after);
  RendererGeode reference = createRenderer();
  reference.draw(fresh);
  const RendererBitmap expected = reference.takeSnapshot();
  ASSERT_THAT(expected.empty(), testing::IsFalse());
  // Premise: the edit changes the rendering, so serving the old outline cannot match. The
  // comparison only measures the difference here, so any count is allowed.
  editor::tests::BitmapGoldenCompareParams premiseParams =
      editor::tests::PixelmatchIdentityParams();
  premiseParams.maxMismatchedPixels = std::numeric_limits<int>::max();
  int editChangedPixels = -1;
  editor::tests::CompareBitmapToBitmap(beforeEdit, expected,
                                       "stroke_key_premise_" + std::string(edit.name),
                                       premiseParams, &editChangedPixels);
  ASSERT_THAT(editChangedPixels, testing::Gt(0))
      << "the edit to " << edit.attribute << " must change the rendering";

  editor::tests::CompareBitmapToBitmap(afterEdit, expected, "stroke_key_" + std::string(edit.name),
                                       editor::tests::PixelmatchIdentityParams());
}

INSTANTIATE_TEST_SUITE_P(
    StrokeKeyComponents, RendererGeodeStrokeKeyTest,
    ::testing::Values(
        StrokeKeyEdit{"width", "edited", "stroke-width", "5", "8", ""},
        StrokeKeyEdit{"linejoin", "edited", "stroke-linejoin", "miter", "bevel",
                      R"(stroke-width="5")"},
        StrokeKeyEdit{"linecap", "edited", "stroke-linecap", "butt", "square",
                      R"(stroke-width="5")"},
        StrokeKeyEdit{"miterlimit", "edited", "stroke-miterlimit", "4", "1", R"(stroke-width="5")"},
        StrokeKeyEdit{"dasharray", "edited", "stroke-dasharray", "6,3", "4,2",
                      R"(stroke-width="5")"},
        StrokeKeyEdit{"dashoffset", "edited", "stroke-dashoffset", "0", "3",
                      R"(stroke-width="5" stroke-dasharray="6,3")"},
        StrokeKeyEdit{"pathlength", "p", "pathLength", "100", "50",
                      R"(stroke-width="5" stroke-dasharray="6,3")"},
        StrokeKeyEdit{"tolerance", "zoom", "transform", "scale(1)", "scale(4)",
                      R"(stroke-width="5")"},
        StrokeKeyEdit{"geometry", "p", "d", "M10,40 L30,10 L50,40 C60,55 75,55 85,40",
                      "M10,40 L30,20 L50,40 C60,65 75,65 85,40", R"(stroke-width="5")"}),
    [](const ::testing::TestParamInfo<StrokeKeyEdit>& info) {
      return std::string(info.param.name);
    });

/// An offscreen pass on the same device inside an open frame, as a filter's image pass is, must
/// not replace a stroke outline that the outer frame's pending batch still draws from. The outer
/// frame leaves one stroke of the path pending; the pass then fills the path's stroke cache with
/// seven more keys, so a ninth key would have to replace the only variant it did not draw.
TEST_F(RendererGeodeTest, NestedPassKeepsTheStrokeOutlineAnOuterFrameLeftPending) {
  const auto strokeOfWidth = [](double width) {
    StrokeParams stroke;
    stroke.strokeWidth = width;
    return stroke;
  };
  const Path curve = PathBuilder().moveTo({8, 40}).curveTo({20, 8}, {44, 8}, {56, 40}).build();
  const css::RGBA red(200, 0, 0, 255);

  RendererGeode reference = createRenderer();
  beginFrame(reference);
  reference.setPaint(solidStroke(red));
  reference.drawPath(PathShape{.path = &curve}, strokeOfWidth(3.0));
  reference.endFrame();
  const RendererBitmap expected = reference.takeSnapshot();
  ASSERT_THAT(expected.empty(), testing::IsFalse());

  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
        <path id="p" d="M8,40 C20,8 44,8 56,40" fill="none"/>
      </svg>)svg",
      warnings);
  ASSERT_THAT(parsed.hasError(), testing::IsFalse());
  SVGDocument document = std::move(parsed).result();
  auto target = document.querySelector("#p");
  ASSERT_THAT(target, testing::Optional(testing::_));
  const EntityHandle source = target->unsafeEntityHandle();

  RendererGeode outer = createRenderer();
  beginFrame(outer);
  outer.setPaint(solidStroke(red));
  outer.drawPath(PathShape{.path = &curve, .sourceEntity = source}, strokeOfWidth(3.0));

  std::unique_ptr<RendererInterface> inner = outer.createOffscreenInstance();
  ASSERT_THAT(inner, testing::NotNull());
  RenderViewport viewport;
  viewport.size = Vector2d(kViewportSize, kViewportSize);
  viewport.devicePixelRatio = 1.0;
  inner->beginFrame(viewport);
  inner->setPaint(solidStroke(css::RGBA(0, 0, 200, 255)));
  for (double width = 4.0; width <= 11.0; width += 1.0) {
    inner->drawPath(PathShape{.path = &curve, .sourceEntity = source}, strokeOfWidth(width));
  }
  inner->endFrame();

  outer.endFrame();
  editor::tests::CompareBitmapToBitmap(outer.takeSnapshot(), expected,
                                       "nested_pass_keeps_pending_stroke_outline",
                                       editor::tests::PixelmatchIdentityParams());
}

/// The fill counterpart of `NestedPassKeepsTheStrokeOutlineAnOuterFrameLeftPending`. The outer
/// frame leaves a fill of the entity pending; the pass then draws the entity at two finer
/// tolerance buckets, which fill the entity's two fill variants, the second by replacement. The
/// pass draws different geometry through the same entity, so an encode replaced under the outer
/// frame's pending batch would show as the wrong shape.
TEST_F(RendererGeodeTest, NestedPassKeepsTheFillEncodeAnOuterFrameLeftPending) {
  const Path outerShape =
      PathBuilder().moveTo({8, 40}).curveTo({20, 8}, {44, 8}, {56, 40}).closePath().build();
  const Path innerShape =
      PathBuilder().moveTo({4, 4}).curveTo({30, 2}, {30, 30}, {4, 30}).closePath().build();
  const css::RGBA red(200, 0, 0, 255);

  RendererGeode reference = createRenderer();
  beginFrame(reference);
  reference.setPaint(solidFill(red));
  reference.drawPath(PathShape{.path = &outerShape}, StrokeParams{});
  reference.endFrame();
  const RendererBitmap expected = reference.takeSnapshot();
  ASSERT_THAT(expected.empty(), testing::IsFalse());

  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
        <path id="p" d="M8,40 C20,8 44,8 56,40 Z" fill="red"/>
      </svg>)svg",
      warnings);
  ASSERT_THAT(parsed.hasError(), testing::IsFalse());
  SVGDocument document = std::move(parsed).result();
  auto target = document.querySelector("#p");
  ASSERT_THAT(target, testing::Optional(testing::_));
  const EntityHandle source = target->unsafeEntityHandle();

  RendererGeode outer = createRenderer();
  beginFrame(outer);
  outer.setPaint(solidFill(red));
  outer.drawPath(PathShape{.path = &outerShape, .sourceEntity = source}, StrokeParams{});

  std::unique_ptr<RendererInterface> inner = outer.createOffscreenInstance();
  ASSERT_THAT(inner, testing::NotNull());
  RenderViewport viewport;
  viewport.size = Vector2d(kViewportSize, kViewportSize);
  viewport.devicePixelRatio = 1.0;
  inner->beginFrame(viewport);
  inner->setPaint(solidFill(css::RGBA(0, 0, 200, 255)));
  for (const double scale : {4.0, 16.0}) {
    inner->setTransform(Transform2d::Scale(scale));
    inner->drawPath(PathShape{.path = &innerShape, .sourceEntity = source}, StrokeParams{});
  }
  inner->endFrame();

  outer.endFrame();
  editor::tests::CompareBitmapToBitmap(outer.takeSnapshot(), expected,
                                       "nested_pass_keeps_pending_fill_encode",
                                       editor::tests::PixelmatchIdentityParams());
}

/// Fill and stroke together: interior should be the fill color, the stroke
/// ring around the edge should be the stroke color.
TEST_F(RendererGeodeTest, FillAndStrokeRect) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  renderer.setPaint(solidFillAndStroke(css::RGBA(0, 255, 0, 255), css::RGBA(0, 0, 255, 255)));
  StrokeParams stroke;
  stroke.strokeWidth = 4.0;

  const Path path = PathBuilder().addRect(Box2d({16, 16}, {48, 48})).build();
  PathShape shape;
  shape.path = &path;
  shape.fillRule = FillRule::NonZero;
  renderer.drawPath(shape, stroke);

  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();

  // Deep inside: fill (green) only.
  auto inside = PixelAt(snap, 32, 32);
  EXPECT_THAT(inside, Rgba(testing::Eq(0), testing::Eq(255), testing::Eq(0), testing::_));

  // Exactly on the top edge of the rect (y=16), the stroke straddles both
  // sides by width/2 = 2, so this pixel is inside the stroke ring → blue.
  auto topEdge = PixelAt(snap, 32, 16);
  EXPECT_THAT(topEdge, Rgba(testing::_, testing::_, testing::Eq(255), testing::_))
      << "Top edge should be in stroke (B)";

  // Far outside the rect is still transparent.
  auto corner = PixelAt(snap, 2, 2);
  EXPECT_THAT(corner, IsTransparent()) << "Corner should be transparent";
}

/// A semi-transparent fill must round-trip through `takeSnapshot` in
/// straight alpha, not premultiplied. Regression guard for #492 review
/// comment P2: `GeoEncoder::fillPath` premultiplies the paint color by
/// alpha before upload (because the pipeline blend state is
/// premultiplied-source-over), so `takeSnapshot` must unpremultiply when
/// building the straight-alpha `RendererBitmap` - otherwise semi-transparent
/// content comes out darkened and cross-backend parity breaks.
TEST_F(RendererGeodeTest, SnapshotReturnsStraightAlpha) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  // 50% red: R=255, A=128. A straight-alpha round-trip should preserve
  // R~=255 and A~=128. A broken read-back would return the premultiplied
  // RGB (~128,0,0,128) instead - exactly the regression we're guarding.
  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 128)));
  renderer.drawRect(Box2d({16, 16}, {48, 48}), StrokeParams{});
  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  auto center = PixelAt(snap, 32, 32);
  EXPECT_THAT(center, Rgba(Near(255, 2), testing::Eq(0), testing::Eq(0), Near(128, 2)))
      << "Straight-alpha color and alpha should be preserved";
}

/// Drawing a stroked path when `impl_->encoder` is null (e.g.,
/// draw-before-beginFrame) must be a safe no-op. Regression guard for
/// #492 review comment P1: the stroke branch of `drawPath` previously
/// dereferenced `impl_->encoder` unconditionally, which crashed when the
/// encoder hadn't been created yet. Before the fix, this test would
/// segfault; after the fix, it returns cleanly.
TEST_F(RendererGeodeTest, StrokeBeforeBeginFrameIsNoOp) {
  RendererGeode renderer = createRenderer();
  // Intentionally skip beginFrame - encoder remains null.
  renderer.setPaint(solidStroke(css::RGBA(255, 0, 0, 255)));
  StrokeParams stroke;
  stroke.strokeWidth = 4.0;

  const Path path = PathBuilder().addRect(Box2d({16, 16}, {48, 48})).build();
  PathShape shape;
  shape.path = &path;
  shape.fillRule = FillRule::NonZero;
  // Before the fix, this call crashes with a null pointer dereference.
  renderer.drawPath(shape, stroke);
  // No explicit assertion - reaching this line means we didn't crash.
}

/// Stroke with stroke-width 0 should no-op (neither stroke nor warning).
TEST_F(RendererGeodeTest, ZeroWidthStrokeIsNoOp) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  renderer.setPaint(solidStroke(css::RGBA(255, 0, 0, 255)));
  StrokeParams stroke;
  stroke.strokeWidth = 0.0;  // Must skip stroke path entirely.

  const Path path = PathBuilder().addRect(Box2d({16, 16}, {48, 48})).build();
  PathShape shape;
  shape.path = &path;
  shape.fillRule = FillRule::NonZero;
  renderer.drawPath(shape, stroke);

  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  auto center = PixelAt(snap, 32, 32);
  EXPECT_THAT(center, IsTransparent()) << "Zero-width stroke should draw nothing";
}

/// `drawImage` must upload the RGBA pixels to a texture and render the
/// textured quad at the target rectangle, honoring the current transform
/// and the combined opacity. Draw a 4-color 2x2 image stretched across
/// most of the canvas and sanity-check the four expected corner colors
/// (nearest-neighbor sampling so quadrant boundaries are crisp).
TEST_F(RendererGeodeTest, DrawImageFourColorQuadrants) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  renderer.setPaint(PaintParams{});

  // 2x2 image: RGBY across the four pixels. Pixelated filtering so each
  // quadrant of the destination rect is a single pure color.
  ImageResource image;
  image.width = 2;
  image.height = 2;
  image.data = {
      255, 0,   0,   255,  // (0,0) red
      0,   255, 0,   255,  // (1,0) green
      0,   0,   255, 255,  // (0,1) blue
      255, 255, 0,   255,  // (1,1) yellow
  };

  ImageParams params;
  params.targetRect = Box2d({16.0, 16.0}, {48.0, 48.0});
  params.imageRendering = ImageRendering::Pixelated;

  renderer.drawImage(image, params);
  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  ASSERT_FALSE(snap.empty());

  // Top-left quadrant (pixel at ~24, 24) - red.
  auto tl = PixelAt(snap, 24, 24);
  EXPECT_THAT(tl, RgbaEq(255, 0, 0, 255)) << "Top-left quadrant should be red";

  // Top-right quadrant (~40, 24) - green.
  auto tr = PixelAt(snap, 40, 24);
  EXPECT_THAT(tr, RgbaEq(0, 255, 0, 255)) << "Top-right quadrant should be green";

  // Bottom-left (~24, 40) - blue.
  auto bl = PixelAt(snap, 24, 40);
  EXPECT_THAT(bl, RgbaEq(0, 0, 255, 255)) << "Bottom-left quadrant should be blue";

  // Bottom-right (~40, 40) - yellow.
  auto br = PixelAt(snap, 40, 40);
  EXPECT_THAT(br, RgbaEq(255, 255, 0, 255)) << "Bottom-right quadrant should be yellow";

  // Outside the target rect: transparent.
  auto outside = PixelAt(snap, 4, 4);
  EXPECT_THAT(outside, IsTransparent()) << "Outside alpha";
}

TEST_F(RendererGeodeTest, DrawImagePixelatedSmoothsFromNearestIntegerScale) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  ImageResource image;
  image.width = 2;
  image.height = 2;
  image.data = {
      255, 0, 0, 255, 0, 0, 255, 255, 255, 0, 0, 255, 0, 0, 255, 255,
  };

  ImageParams params;
  params.targetRect = Box2d({16.0, 16.0}, {21.0, 21.0});
  params.imageRendering = ImageRendering::Pixelated;

  renderer.drawImage(image, params);
  renderer.endFrame();

  const std::array<uint8_t, 4> center = PixelAt(renderer.takeSnapshot(), 18, 18);
  EXPECT_THAT(center, Rgba(::testing::Gt(0), 0, ::testing::Gt(0), 255));
}

TEST_F(RendererGeodeTest, DrawImagePixelatedInterpolatesPremultipliedColors) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  ImageResource image;
  image.width = 2;
  image.height = 1;
  image.data = {255, 0, 0, 255, 0, 0, 255, 0};

  ImageParams params;
  params.targetRect = Box2d({16.0, 16.0}, {21.0, 17.0});
  params.imageRendering = ImageRendering::Pixelated;
  renderer.drawImage(image, params);
  renderer.endFrame();

  const std::array<uint8_t, 4> center = PixelAt(renderer.takeSnapshot(), 18, 16);
  EXPECT_THAT(center, Rgba(Near(255, 2), 0, 0, Near(128, 8)));
}

/// Transform stack must compose with drawImage. Push a translate, draw
/// the image at the *unshifted* targetRect, pop, and verify the image
/// lands at the translated position.
TEST_F(RendererGeodeTest, DrawImageHonorsTransformStack) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  ImageResource image;
  image.width = 1;
  image.height = 1;
  image.data = {255, 0, 255, 255};  // Solid magenta.

  ImageParams params;
  params.targetRect = Box2d({0.0, 0.0}, {8.0, 8.0});

  renderer.pushTransform(Transform2d::Translate(Vector2d(16, 16)));
  renderer.drawImage(image, params);
  renderer.popTransform();

  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  // Translated pixel should be magenta.
  auto inside = PixelAt(snap, 20, 20);
  EXPECT_THAT(inside, RgbaEq(255, 0, 255, 255)) << "Translated image should be magenta";

  // Original (unshifted) position should be empty.
  auto unshifted = PixelAt(snap, 4, 4);
  EXPECT_THAT(unshifted, IsTransparent()) << "Unshifted origin should be transparent";
}

/// Image opacity belongs to the driver's isolated layer. Setting paint opacity alone must not
/// attenuate a direct image draw.
TEST_F(RendererGeodeTest, DrawImageIgnoresPaintOpacity) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  PaintParams paint;
  paint.opacity = 0.5;
  renderer.setPaint(paint);

  ImageResource image;
  image.width = 1;
  image.height = 1;
  image.data = {255, 0, 0, 255};

  ImageParams params;
  params.targetRect = Box2d({16.0, 16.0}, {48.0, 48.0});

  renderer.drawImage(image, params);
  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  auto center = PixelAt(snap, 32, 32);
  EXPECT_THAT(center, RgbaEq(255, 0, 0, 255));
}

/// Empty image data (width/height = 0) and a zero-size target rect both
/// must be safe no-ops.
TEST_F(RendererGeodeTest, DrawImageEmptyIsNoOp) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  ImageResource empty;
  empty.width = 0;
  empty.height = 0;
  ImageParams params;
  params.targetRect = Box2d({16.0, 16.0}, {48.0, 48.0});
  renderer.drawImage(empty, params);

  ImageResource valid;
  valid.width = 1;
  valid.height = 1;
  valid.data = {255, 255, 255, 255};
  ImageParams emptyRect;
  emptyRect.targetRect = Box2d({16.0, 16.0}, {16.0, 16.0});
  renderer.drawImage(valid, emptyRect);

  renderer.endFrame();
  RendererBitmap snap = renderer.takeSnapshot();
  auto center = PixelAt(snap, 32, 32);
  EXPECT_THAT(center, IsTransparent()) << "Empty image should draw nothing";
}

TEST_F(RendererGeodeTest, DrawImageWithTrailingPayloadIsNoOp) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  ImageResource image;
  image.width = 1;
  image.height = 1;
  image.data = {255, 0, 0, 255, 17};
  ImageParams params;
  params.targetRect = Box2d({16.0, 16.0}, {48.0, 48.0});
  renderer.drawImage(image, params);
  renderer.endFrame();

  EXPECT_THAT(PixelAt(renderer.takeSnapshot(), 32, 32), IsTransparent());
}

/// Popping an isolated layer with a non-Normal blend mode while an outer
/// clip is active must NOT clobber backdrop pixels outside the clip rect.
/// This is the blend-mode regression guarded by loading (not clearing) the
/// parent attachment on the blend-pop pass - a clear would wipe
/// out-of-scissor pixels to transparent since the blend blit runs only
/// inside the scissor.
TEST_F(RendererGeodeTest, BlendedLayerPopPreservesBackdropOutsideClip) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  // Backdrop: fill the whole canvas red.
  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  renderer.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});

  // Clip to the top-right quadrant (32..64, 0..32) and inside that, push an
  // isolated layer with mix-blend-mode:Multiply, draw blue, then pop.
  ResolvedClip clip;
  clip.clipRect = Box2d({32, 0}, {kViewportSize, 32});
  renderer.pushClip(clip);
  renderer.pushIsolatedLayer(1.0, MixBlendMode::Multiply);
  renderer.setPaint(solidFill(css::RGBA(0, 0, 255, 255)));
  renderer.drawRect(Box2d({32, 0}, {kViewportSize, 32}), StrokeParams{});
  renderer.popIsolatedLayer();
  renderer.popClip();

  renderer.endFrame();
  RendererBitmap snap = renderer.takeSnapshot();

  // Outside the clip (bottom-left quadrant): backdrop must still be red.
  auto outside = PixelAt(snap, 16, 48);
  EXPECT_THAT(outside, RgbaEq(255, 0, 0, 255))
      << "Bottom-left pixel outside clip should preserve backdrop";

  // Inside the clip: Multiply(red=255,0,0 ; blue=0,0,255) = (0, 0, 0).
  auto inside = PixelAt(snap, 48, 16);
  EXPECT_THAT(inside, RgbaEq(0, 0, 0, 255)) << "Inside clip should multiply to black";
}

/// Repro: an arbitrary path clip should gate color draws to the mask.
/// A full-viewport blue rect clipped by a left-half rect encoded as a PATH
/// must render blue only on the left half.
TEST_F(RendererGeodeTest, PathClipMaskClipsSolidFillToLeftHalf) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  ResolvedClip clip;
  ClipPathShape clipShape;
  clipShape.fillRule = FillRule::NonZero;
  clipShape.path = PathBuilder()
                       .moveTo(Vector2d(0.0, 0.0))
                       .lineTo(Vector2d(32.0, 0.0))
                       .lineTo(Vector2d(32.0, kViewportSize))
                       .lineTo(Vector2d(0.0, kViewportSize))
                       .closePath()
                       .build();
  clip.clipPaths.push_back(std::move(clipShape));

  renderer.pushClip(clip);
  renderer.setPaint(solidFill(css::RGBA(0, 0, 255, 255)));
  renderer.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});
  renderer.popClip();

  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  ASSERT_FALSE(snap.empty());

  auto inside = PixelAt(snap, 16, 32);
  EXPECT_THAT(inside, RgbaEq(0, 0, 255, 255)) << "Inside clip should be blue";

  auto outside = PixelAt(snap, 48, 32);
  EXPECT_THAT(outside, IsTransparent()) << "Outside clip should be transparent";
}

TEST_F(RendererGeodeTest, PathClipMaskClipsIsolatedLayerComposite) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  ResolvedClip clip;
  ClipPathShape clipShape;
  clipShape.fillRule = FillRule::NonZero;
  clipShape.path = PathBuilder()
                       .moveTo(Vector2d(0.0, 0.0))
                       .lineTo(Vector2d(32.0, 0.0))
                       .lineTo(Vector2d(32.0, kViewportSize))
                       .lineTo(Vector2d(0.0, kViewportSize))
                       .closePath()
                       .build();
  clip.clipPaths.push_back(std::move(clipShape));

  renderer.pushClip(clip);
  renderer.pushIsolatedLayer(1.0, MixBlendMode::Normal);
  renderer.setPaint(solidFill(css::RGBA(0, 0, 255, 255)));
  renderer.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});
  renderer.popIsolatedLayer();
  renderer.popClip();

  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  ASSERT_FALSE(snap.empty());

  auto inside = PixelAt(snap, 16, 32);
  EXPECT_THAT(inside, RgbaEq(0, 0, 255, 255)) << "Inside clip should be blue";

  auto outside = PixelAt(snap, 48, 32);
  EXPECT_THAT(outside, IsTransparent()) << "Outside clip should be transparent";
}

TEST_F(RendererGeodeTest, PathClipMaskClipsIsolatedLayerCompositeForTriangle) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  ResolvedClip clip;
  ClipPathShape clipShape;
  clipShape.fillRule = FillRule::NonZero;
  clipShape.path = PathBuilder()
                       .moveTo(Vector2d(32.0, 0.0))
                       .lineTo(Vector2d(kViewportSize, kViewportSize))
                       .lineTo(Vector2d(0.0, kViewportSize))
                       .closePath()
                       .build();
  clip.clipPaths.push_back(std::move(clipShape));

  renderer.pushClip(clip);
  renderer.pushIsolatedLayer(1.0, MixBlendMode::Normal);
  renderer.setPaint(solidFill(css::RGBA(0, 0, 255, 255)));
  renderer.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});
  renderer.popIsolatedLayer();
  renderer.popClip();

  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  ASSERT_FALSE(snap.empty());

  auto inside = PixelAt(snap, 32, 40);
  EXPECT_THAT(inside, RgbaEq(0, 0, 255, 255)) << "Inside clip should be blue";

  auto outside = PixelAt(snap, 8, 8);
  EXPECT_THAT(outside, IsTransparent()) << "Outside clip should be transparent";
}

TEST_F(RendererGeodeTest, PathClipMaskClipsFilterLayerCompositeForTriangle) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  ResolvedClip clip;
  ClipPathShape clipShape;
  clipShape.fillRule = FillRule::NonZero;
  clipShape.path = PathBuilder()
                       .moveTo(Vector2d(32.0, 0.0))
                       .lineTo(Vector2d(kViewportSize, kViewportSize))
                       .lineTo(Vector2d(0.0, kViewportSize))
                       .closePath()
                       .build();
  clip.clipPaths.push_back(std::move(clipShape));

  renderer.pushClip(clip);
  renderer.pushFilterLayer(components::FilterGraph{}, std::nullopt);
  renderer.setPaint(solidFill(css::RGBA(0, 0, 255, 255)));
  renderer.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});
  renderer.popFilterLayer();
  renderer.popClip();

  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  ASSERT_FALSE(snap.empty());

  auto inside = PixelAt(snap, 32, 40);
  EXPECT_THAT(inside, RgbaEq(0, 0, 255, 255)) << "Inside clip should be blue";

  auto outside = PixelAt(snap, 8, 8);
  EXPECT_THAT(outside, IsTransparent()) << "Outside clip should be transparent";
}

/// Stubbed methods (clip/mask/layer/filter/pattern/image/text) should be
/// safe no-ops that don't crash, and balanced push/pop pairs should keep
/// drawing functional.
TEST_F(RendererGeodeTest, StubbedMethodsAreNoOps) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  // Drive every stub once.
  renderer.pushClip(ResolvedClip{});
  renderer.popClip();
  renderer.pushIsolatedLayer(0.5, MixBlendMode::Normal);
  renderer.popIsolatedLayer();
  renderer.pushMask(std::nullopt, MaskType::Luminance);
  renderer.transitionMaskToContent();
  renderer.popMask();

  // After all that, a normal draw should still work.
  renderer.setPaint(solidFill(css::RGBA(255, 255, 0, 255)));
  renderer.drawRect(Box2d({16, 16}, {48, 48}), StrokeParams{});

  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  auto center = PixelAt(snap, 32, 32);
  EXPECT_THAT(center, RgbaEq(255, 255, 0, 255));
}

/// Smoke test for the feGaussianBlur filter layer. Draws a crisp red
/// rect, wraps it in a Gaussian blur filter, and verifies that edge pixels
/// are blurred (reduced alpha compared to center).
TEST_F(RendererGeodeTest, GaussianBlurSmokes) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  // Build a simple filter graph with a single GaussianBlur node.
  components::FilterGraph graph;
  components::FilterNode blurNode;
  components::filter_primitive::GaussianBlur blur;
  blur.stdDeviationX = 4.0;
  blur.stdDeviationY = 4.0;
  blur.edgeMode = components::filter_primitive::GaussianBlur::EdgeMode::None;
  blurNode.primitive = blur;
  blurNode.inputs.push_back(components::FilterStandardInput::SourceGraphic);
  graph.nodes.push_back(blurNode);

  // Push the filter layer, draw a crisp rect, pop the filter layer.
  renderer.pushFilterLayer(graph, Box2d({0, 0}, {kViewportSize, kViewportSize}));

  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  renderer.setTransform(Transform2d());
  renderer.drawRect(Box2d({16, 16}, {48, 48}), StrokeParams{});

  renderer.popFilterLayer();

  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  ASSERT_FALSE(snap.empty());

  // Center pixel should still be red (fully opaque).
  auto center = PixelAt(snap, 32, 32);
  EXPECT_THAT(center, Rgba(testing::Eq(255), testing::_, testing::_, testing::Gt(200)))
      << "Center pixel should stay red and mostly opaque";

  // An edge pixel (just inside the rect boundary) should have reduced
  // alpha compared to the center, proving the blur was applied.
  auto edge = PixelAt(snap, 16, 32);
  EXPECT_THAT(edge, Alpha(testing::Lt(center[3])))
      << "Edge pixel alpha should be less than center (blur applied)";
}

/// Filter layer with zero stdDeviation should pass through unchanged.
TEST_F(RendererGeodeTest, GaussianBlurZeroStdDevPassthrough) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  components::FilterGraph graph;
  components::FilterNode blurNode;
  components::filter_primitive::GaussianBlur blur;
  blur.stdDeviationX = 0.0;
  blur.stdDeviationY = 0.0;
  blurNode.primitive = blur;
  blurNode.inputs.push_back(components::FilterStandardInput::SourceGraphic);
  graph.nodes.push_back(blurNode);

  renderer.pushFilterLayer(graph, Box2d({0, 0}, {kViewportSize, kViewportSize}));

  renderer.setPaint(solidFill(css::RGBA(0, 255, 0, 255)));
  renderer.setTransform(Transform2d());
  renderer.drawRect(Box2d({16, 16}, {48, 48}), StrokeParams{});

  renderer.popFilterLayer();

  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  ASSERT_FALSE(snap.empty());

  // Center should be green (the blur is a passthrough).
  auto center = PixelAt(snap, 32, 32);
  EXPECT_THAT(center, RgbaEq(0, 255, 0, 255)) << "Zero blur should pass through green";

  // Just outside the rect should be transparent (no blur spread).
  auto outside = PixelAt(snap, 14, 32);
  EXPECT_THAT(outside, IsTransparent()) << "Outside the rect should be transparent with zero blur";
}

/// Zero-deviation blur with an explicit reduced subregion must still clip
/// the primitive output to that subregion. The clip fold skips zero-pass
/// blurs (no dispatch would carry the folded clip), so the dedicated
/// subregion clip pass must run; this pins that fallback.
TEST_F(RendererGeodeTest, GaussianBlurZeroStdDevClipsToExplicitSubregion) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  components::FilterGraph graph;
  components::FilterNode blurNode;
  components::filter_primitive::GaussianBlur blur;
  blur.stdDeviationX = 0.0;
  blur.stdDeviationY = 0.0;
  blurNode.primitive = blur;
  blurNode.inputs.push_back(components::FilterStandardInput::SourceGraphic);
  // Explicit subregion: only the left half of the filter region survives.
  blurNode.x = Lengthd(0.0, Lengthd::Unit::Percent);
  blurNode.y = Lengthd(0.0, Lengthd::Unit::Percent);
  blurNode.width = Lengthd(50.0, Lengthd::Unit::Percent);
  blurNode.height = Lengthd(100.0, Lengthd::Unit::Percent);
  graph.nodes.push_back(blurNode);

  renderer.pushFilterLayer(graph, Box2d({0, 0}, {kViewportSize, kViewportSize}));

  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  renderer.setTransform(Transform2d());
  renderer.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});

  renderer.popFilterLayer();
  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  ASSERT_FALSE(snap.empty());

  // Inside the subregion: red (zero blur passes through).
  auto inside = PixelAt(snap, 16, 32);
  EXPECT_THAT(inside, RgbaEq(255, 0, 0, 255))
      << "Zero-deviation blur inside the subregion should pass through";

  // Outside the subregion: transparent (the clip must still apply).
  auto outside = PixelAt(snap, 48, 32);
  EXPECT_THAT(outside, IsTransparent())
      << "Zero-deviation blur must still clip to the explicit subregion";
}

/// feOffset: shift a red rect by (4, 4) and verify pixels moved.
TEST_F(RendererGeodeTest, FilterOffsetShiftsPixels) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  components::FilterGraph graph;
  components::FilterNode offsetNode;
  components::filter_primitive::Offset offset;
  offset.dx = 4.0;
  offset.dy = 4.0;
  offsetNode.primitive = offset;
  offsetNode.inputs.push_back(components::FilterStandardInput::SourceGraphic);
  graph.nodes.push_back(offsetNode);

  renderer.pushFilterLayer(graph, Box2d({0, 0}, {kViewportSize, kViewportSize}));

  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  renderer.setTransform(Transform2d());
  renderer.drawRect(Box2d({16, 16}, {48, 48}), StrokeParams{});

  renderer.popFilterLayer();
  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  ASSERT_FALSE(snap.empty());

  // After offset dx=4 dy=4, the rect should shift: center moves from (32,32)
  // to effectively be at (32,32) still red (inside shifted rect), but (16,16)
  // should now be transparent (original top-left shifted away).
  auto shifted = PixelAt(snap, 36, 36);
  EXPECT_THAT(shifted, RgbaEq(255, 0, 0, 255)) << "Shifted center should be red";

  // Original corner of the rect at (17,17) should now be transparent because
  // the offset shifted content down-right.
  auto original = PixelAt(snap, 17, 17);
  EXPECT_THAT(original, IsTransparent()) << "Original top-left should be transparent after offset";
}

/// feOffset with a shift that differs along the two axes, so a pass that transposes them is
/// visible. The symmetric case above cannot see that: swapping its two equal components is the
/// identity.
TEST_F(RendererGeodeTest, FilterOffsetKeepsTheAxesApart) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  components::FilterGraph graph;
  components::FilterNode offsetNode;
  components::filter_primitive::Offset offset;
  offset.dx = 6.0;
  offset.dy = -3.0;
  offsetNode.primitive = offset;
  offsetNode.inputs.push_back(components::FilterStandardInput::SourceGraphic);
  graph.nodes.push_back(offsetNode);

  renderer.pushFilterLayer(graph, Box2d({0, 0}, {kViewportSize, kViewportSize}));

  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  renderer.setTransform(Transform2d());
  renderer.drawRect(Box2d({16, 16}, {48, 48}), StrokeParams{});

  renderer.popFilterLayer();
  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  ASSERT_FALSE(snap.empty());

  // The rect moves to [22, 13] - [54, 45]. Transposing the shift would move it to
  // [13, 22] - [45, 54] instead, which contains neither of these two texels nor misses them.
  EXPECT_THAT(PixelAt(snap, 52, 20), RgbaEq(255, 0, 0, 255))
      << "A texel inside the rect shifted right and up must be red";
  EXPECT_THAT(PixelAt(snap, 20, 50), IsTransparent())
      << "A texel the transposed shift would have covered must be clear";
}

/// feOffset with a shift on an exact half translates by that half pixel instead of snapping.
TEST_F(RendererGeodeTest, FilterOffsetTranslatesByAHalfPixel) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  components::FilterGraph graph;
  components::FilterNode offsetNode;
  components::filter_primitive::Offset offset;
  offset.dx = 2.5;
  offset.dy = 0.0;
  offsetNode.primitive = offset;
  offsetNode.inputs.push_back(components::FilterStandardInput::SourceGraphic);
  graph.nodes.push_back(offsetNode);

  renderer.pushFilterLayer(graph, Box2d({0, 0}, {kViewportSize, kViewportSize}));

  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  renderer.setTransform(Transform2d());
  renderer.drawRect(Box2d({16, 16}, {48, 48}), StrokeParams{});

  renderer.popFilterLayer();
  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  ASSERT_FALSE(snap.empty());

  // The shifted right edge lands at 50.5: texel 49 stays wholly inside, texel 50 is half covered,
  // and texel 51 is outside. Snapping the shift to either neighboring whole pixel would leave
  // texel 50 wholly inside or wholly outside.
  EXPECT_THAT(PixelAt(snap, 49, 32), RgbaEq(255, 0, 0, 255));
  EXPECT_THAT(PixelAt(snap, 50, 32), RgbaEq(255, 0, 0, 128));
  EXPECT_THAT(PixelAt(snap, 51, 32), IsTransparent());
}

/// Parses \p source, which must parse without warnings, into \p document.
/// @param source SVG document text.
/// @param document Receives the parsed document.
void ParseDocumentWithoutWarnings(std::string_view source, std::optional<SVGDocument>& document) {
  ParseWarningSink warnings;
  ParseResult<SVGDocument> parsed = parser::SVGParser::ParseSVG(source, warnings);
  ASSERT_THAT(parsed, NoParseError());
  EXPECT_THAT(warnings.warnings(), testing::IsEmpty());
  document = std::move(parsed.result());
}

/// Renders \p document through tiny-skia, the reference the Geode renderings below are held to.
/// @param document Parsed document.
RendererBitmap RenderWithTinySkia(SVGDocument& document) {
  RendererTinySkia reference;
  reference.draw(document);
  return reference.takeSnapshot();
}

TEST_F(RendererGeodeTest, NegativeFilterAmountsPreserveUnfilteredPixels) {
  std::optional<SVGDocument> referenceDocument;
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
        <rect width="8" height="8" fill="blue"/>
      </svg>)",
      referenceDocument));
  const RendererBitmap expected = RenderWithTinySkia(*referenceDocument);
  EXPECT_THAT(PixelAt(expected, 4, 4), RgbaEq(0, 0, 255, 255));

  for (std::string_view function : {"grayscale", "invert", "opacity", "sepia"}) {
    SCOPED_TRACE(function);
    const std::string source =
        R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
          <rect width="8" height="8" fill="blue" filter=")svg" +
        std::string(function) + R"svg((-1)"/></svg>)svg";
    ParseWarningSink warnings;
    auto parsed = parser::SVGParser::ParseSVG(source, warnings);
    ASSERT_THAT(parsed, NoParseError());
    EXPECT_THAT(warnings.warnings(), testing::Not(testing::IsEmpty()));

    editor::tests::CompareBitmapToBitmap(RenderWithTinySkia(parsed.result()), expected,
                                         "negative_filter_cpu_" + std::string(function),
                                         editor::tests::PixelmatchIdentityParams());
    RendererGeode renderer = createRenderer();
    renderer.draw(parsed.result());
    EXPECT_THAT(renderer.deviceLost(), testing::IsFalse());
    editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), expected,
                                         "negative_filter_geode_" + std::string(function),
                                         editor::tests::PixelmatchIdentityParams());
  }
}

/// How a Geode rendering of a scene with a circle is held to tiny-skia's. Scenes without one
/// match pixel for pixel and use \ref editor::tests::PixelmatchIdentityParams. In scenes with a
/// circle the two agree exactly on every pixel the circle's edge does not cross. Along the edge,
/// the coverage of partially covered pixels differs between them: every differing pixel lies
/// within a pixel of the edge, with alpha up to 56 levels apart. The comparison therefore uses the
/// renderer suite's perceptual threshold, excludes the pixels pixelmatch classifies as
/// anti-aliased, and allows no other mismatch.
constexpr editor::tests::BitmapGoldenCompareParams kCircleEdgeParams =
    editor::tests::ApprovedPixelToleranceParams(0.02f, 0, /*includeAntiAliasing=*/false);

/// Renders \p source through Geode and requires the renderer to keep its device and match
/// tiny-skia's rendering of \p reference. The renderer leases a pooled headless device, which the
/// pool drops once it is lost, so a loss this render declares cannot reach the device the other
/// tests share.
/// @param source SVG document text Geode renders.
/// @param reference SVG document text of the same image, which tiny-skia renders.
/// @param label Comparison label naming the diff images written on a mismatch.
/// @param params How closely the two renderings must match.
void ExpectGeodeMatchesTinySkia(std::string_view source, std::string_view reference,
                                std::string_view label,
                                const editor::tests::BitmapGoldenCompareParams& params) {
  std::optional<SVGDocument> document;
  std::optional<SVGDocument> referenceDocument;
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(source, document));
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(reference, referenceDocument));
  RendererGeode renderer;
  renderer.draw(*document);
  EXPECT_THAT(renderer.deviceLost(), testing::IsFalse());
  editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(),
                                       RenderWithTinySkia(*referenceDocument), label, params);
}

/// A clip-path on an element inside an feOffset filter whose region starts above and left of the
/// surface. The filter captures into a layer larger than the surface by that overhang, so the
/// clip's coverage mask has to take the layer's extent: a mask pass scissored to the layer over a
/// surface-sized mask is invalid, and a surface-sized mask cannot cover the whole layer.
TEST_F(RendererGeodeTest, ClipPathInsideAnExpandedFilterLayerMatchesTinySkia) {
  constexpr std::string_view kSource =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200"><clipPath id="c"><circle cx="100" cy="100" r="80"/></clipPath><filter id="f" filterUnits="userSpaceOnUse" x="-20" y="-20" width="240" height="240"><feOffset dx="4" dy="4"/></filter><g filter="url(#f)"><rect width="200" height="200" fill="blue" clip-path="url(#c)"/></g></svg>)svg";
  ExpectGeodeMatchesTinySkia(kSource, kSource, "clip_path_in_expanded_filter_layer",
                             kCircleEdgeParams);
}

/// \p count evenly spaced `<stop>` elements, each a different color.
std::string GradientStopElements(int count) {
  std::string stops;
  for (int i = 0; i < count; ++i) {
    char stop[64];
    std::snprintf(stop, sizeof(stop), R"(<stop offset="%.6f" stop-color="#%02x%02x%02x"/>)",
                  static_cast<double>(i) / (count - 1), (i * 67 + 13) % 256, (i * 151 + 101) % 256,
                  (i * 29 + 200) % 256);
    stops += stop;
  }
  return stops;
}

/// Builds a document whose gradient has \p stopCount evenly spaced stops, each a different color,
/// and paints it three ways: a fill, a stroke, and a fill under a clip path that covers the whole
/// canvas. The clip path is a triangle rather than a rectangle so it renders through a coverage
/// mask, which keeps that fill out of the cross-entity batches and the per-entity residence the
/// other two can use; the mask covers every pixel fully, so it changes no pixel.
/// @param stopCount Number of `<stop>` elements.
/// @param radial True for a radialGradient, false for a linearGradient.
std::string ManyStopGradientDocument(int stopCount, bool radial) {
  std::string source =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="128" height="48"><defs>)svg"
      R"svg(<clipPath id="cover"><path d="M-200,-200 L400,-200 L-200,400 Z"/></clipPath>)svg";
  source +=
      radial
          ? R"svg(<radialGradient id="g" gradientUnits="userSpaceOnUse" cx="40" cy="24" r="80">)svg"
          : R"svg(<linearGradient id="g" gradientUnits="userSpaceOnUse" x1="4" y1="0" x2="124" y2="0">)svg";
  source += GradientStopElements(stopCount);
  source += radial ? "</radialGradient>" : "</linearGradient>";
  source +=
      R"svg(</defs><rect width="128" height="16" fill="url(#g)"/>)svg"
      R"svg(<path d="M0,24 H128" stroke="url(#g)" stroke-width="16" fill="none"/>)svg"
      R"svg(<rect y="32" width="128" height="16" fill="url(#g)" clip-path="url(#cover)"/></svg>)svg";
  return source;
}

/// How a Geode rendering of a steep gradient is held to tiny-skia's. In the 2500-stop gradients
/// there are about 20 to 30 stops per pixel and neighboring stops differ by up to 105 levels in a
/// channel, so the color changes by thousands of levels per pixel; in the radial spread cases, 19
/// such steps span 20 pixels. The two renderers compute the gradient position in different float
/// arithmetic, and a difference far below a pixel moves a channel by one level: on the software
/// Vulkan lane about 2 percent of the 2500-stop radial gradient's pixels, and 10 pixels of each
/// radial spread case, including pixels inside the gradient's first period, differ by exactly one
/// level. The renderer suite's default threshold admits those one-level differences, and no pixel,
/// anti-aliased or not, may exceed it.
constexpr editor::tests::BitmapGoldenCompareParams kSteepGradientParams =
    editor::tests::ApprovedPixelToleranceParams(0.02f, 0, /*includeAntiAliasing=*/true);

/// A gradient renders every one of its stops, however many it has, in each of the ways Geode
/// draws a gradient. 16 is the most stops a gradient's draw parameters carry inline, so the cases
/// sit on both sides of it, and the largest spans several rows of the texture that holds stops
/// past that.
TEST_F(RendererGeodeTest, GradientsWithManyStopsMatchTinySkia) {
  for (const bool radial : {false, true}) {
    for (const int stopCount : {16, 17, 32, 2500}) {
      const std::string label = std::string(radial ? "radial" : "linear") + "_gradient_" +
                                std::to_string(stopCount) + "_stops";
      SCOPED_TRACE(label);
      const std::string source = ManyStopGradientDocument(stopCount, radial);
      ExpectGeodeMatchesTinySkia(
          source, source, label,
          stopCount > 32 ? kSteepGradientParams : editor::tests::PixelmatchIdentityParams());
    }
  }
}

/// Wraps \p body in a 128 by 48 document whose defs hold \p defs.
std::string GradientTestDocument(std::string_view defs, std::string_view body) {
  return std::string(
             R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="128" height="48"><defs>)svg") +
         std::string(defs) + "</defs>" + std::string(body) + "</svg>";
}

/// Ten stripes from 20 stops: each stripe's two stops share a color, and each stripe ends at the
/// offset where the next begins, so every boundary is a hard edge between two stops at one
/// offset. The edges fall on whole pixels, between pixel centers.
TEST_F(RendererGeodeTest, ManyStopHardEdgesMatchTinySkia) {
  std::string stops;
  for (int stripe = 0; stripe < 10; ++stripe) {
    char color[8];
    std::snprintf(color, sizeof(color), "#%02x%02x%02x", (stripe * 97 + 20) % 256,
                  (stripe * 53 + 90) % 256, (stripe * 31 + 160) % 256);
    for (const int edge : {stripe, stripe + 1}) {
      stops +=
          "<stop offset=\"" + std::to_string(edge / 10.0) + "\" stop-color=\"" + color + "\"/>";
    }
  }
  for (const bool radial : {false, true}) {
    const std::string label = std::string(radial ? "radial" : "linear") + "_hard_edges";
    SCOPED_TRACE(label);
    const std::string gradient =
        radial
            ? R"svg(<radialGradient id="g" gradientUnits="userSpaceOnUse" cx="4" cy="24" r="120">)svg" +
                  stops + "</radialGradient>"
            : R"svg(<linearGradient id="g" gradientUnits="userSpaceOnUse" x1="4" y1="0" x2="124" y2="0">)svg" +
                  stops + "</linearGradient>";
    const std::string source =
        GradientTestDocument(gradient, R"svg(<rect width="128" height="48" fill="url(#g)"/>)svg");
    ExpectGeodeMatchesTinySkia(source, source, label, editor::tests::PixelmatchIdentityParams());
  }
}

/// Reflect and repeat spread with a stop ramp: the 20-stop gradient covers a quarter of the rect,
/// so the rest of it is the ramp mirrored or repeated.
TEST_F(RendererGeodeTest, ManyStopSpreadMethodsMatchTinySkia) {
  const std::string stops = GradientStopElements(20);
  for (const std::string_view spread : {"reflect", "repeat"}) {
    for (const bool radial : {false, true}) {
      const std::string label =
          std::string(radial ? "radial_" : "linear_") + std::string(spread) + "_spread";
      SCOPED_TRACE(label);
      const std::string open =
          radial
              ? R"svg(<radialGradient id="g" gradientUnits="userSpaceOnUse" cx="40" cy="24" r="20" spreadMethod=")svg"
              : R"svg(<linearGradient id="g" gradientUnits="userSpaceOnUse" x1="40" y1="0" x2="72" y2="0" spreadMethod=")svg";
      const std::string gradient = open + std::string(spread) + "\">" + stops +
                                   (radial ? "</radialGradient>" : "</linearGradient>");
      const std::string source =
          GradientTestDocument(gradient, R"svg(<rect width="128" height="48" fill="url(#g)"/>)svg");
      ExpectGeodeMatchesTinySkia(
          source, source, label,
          radial ? kSteepGradientParams : editor::tests::PixelmatchIdentityParams());
    }
  }
}

/// A gradient with too many stops to batch, drawn between two overlapping solid fills that can,
/// keeps its place in painter order: it covers the first fill and the second covers it.
TEST_F(RendererGeodeTest, ManyStopGradientBetweenBatchedFillsKeepsPaintOrder) {
  const std::string stops = GradientStopElements(20);
  const std::string source = GradientTestDocument(
      R"svg(<linearGradient id="g" gradientUnits="userSpaceOnUse" x1="32" y1="0" x2="96" y2="0">)svg" +
          stops + "</linearGradient>",
      R"svg(<rect width="64" height="48" fill="#d02020"/>)svg"
      R"svg(<rect x="32" width="64" height="48" fill="url(#g)"/>)svg"
      R"svg(<rect x="80" width="48" height="48" fill="#2020d0"/>)svg");
  ExpectGeodeMatchesTinySkia(source, source, "many_stop_gradient_paint_order",
                             editor::tests::PixelmatchIdentityParams());
}

/// The frame's gradient stops are charged under the CPU renderer's per-frame cap. Past it the draw
/// is refused and so is the rest of the frame, on Geode as on tiny-skia: the same document under
/// the same lowered cap renders the same pixels and the same verdict on both. The 3-stop gradient
/// draws through the fill batch and the 20-stop linear and radial ones through the gradient
/// pipeline, so all of them charge. The 4-stop radial gradient first has identical start and end
/// circles: it draws nothing, but tiny-skia charges its stops before it tests the circles, so Geode
/// charges them too.
TEST_F(RendererGeodeTest, GradientStopBudgetRefusesLikeTinySkia) {
  const std::string twentyStops = GradientStopElements(20);
  const std::string source = GradientTestDocument(
      R"svg(<radialGradient id="focal" r="0.5" fr="0.5"><stop stop-color="red"/><stop offset="0.3" stop-color="lime"/>)svg"
      R"svg(<stop offset="0.6" stop-color="blue"/><stop offset="1" stop-color="yellow"/></radialGradient>)svg"
      R"svg(<linearGradient id="small"><stop stop-color="red"/><stop offset="0.5" stop-color="lime"/><stop offset="1" stop-color="blue"/></linearGradient>)svg"
      R"svg(<linearGradient id="a">)svg" +
          twentyStops + R"svg(</linearGradient><radialGradient id="b">)svg" + twentyStops +
          "</radialGradient>",
      R"svg(<rect width="32" height="48" fill="url(#focal)"/>)svg"
      R"svg(<rect width="32" height="48" fill="url(#small)"/>)svg"
      R"svg(<rect x="32" width="32" height="48" fill="url(#a)"/>)svg"
      R"svg(<rect x="64" width="32" height="48" fill="url(#b)"/>)svg"
      R"svg(<rect x="96" width="32" height="48" fill="#2020d0"/>)svg");
  // 47 admits every gradient; 46 refuses the second 20-stop one, 26 the first, and 3 the
  // one with identical circles.
  for (const std::size_t limit : {47u, 46u, 26u, 3u}) {
    SCOPED_TRACE(limit);
    std::optional<SVGDocument> document;
    std::optional<SVGDocument> referenceDocument;
    ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(source, document));
    ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(source, referenceDocument));
    RendererTinySkia reference;
    reference.setGradientStopBudgetForTesting(limit);
    reference.draw(*referenceDocument);
    RendererGeode renderer;
    renderer.setGradientStopBudgetForTesting(limit);
    renderer.draw(*document);
    EXPECT_THAT(renderer.deviceLost(), testing::IsFalse());
    const RendererResourceStats stats = renderer.resourceStats();
    const RendererResourceStats referenceStats = reference.resourceStats();
    EXPECT_EQ(referenceStats.drawBudgetRejected, limit < 47u);
    EXPECT_EQ(stats.drawBudgetRejected, referenceStats.drawBudgetRejected);
    EXPECT_EQ(stats.gradientStops, referenceStats.gradientStops);
    editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), reference.takeSnapshot(),
                                         "gradient_stop_budget_" + std::to_string(limit),
                                         editor::tests::PixelmatchIdentityParams());
  }
}

/// A resident many-stop gradient keeps its stop ramp across frames, and a stop edit replaces it:
/// each frame matches tiny-skia's rendering of the document as it stands.
TEST_F(RendererGeodeTest, ManyStopGradientStopEditReplacesTheResidentRamp) {
  const std::string source = ManyStopGradientDocument(20, /*radial=*/false);
  std::optional<SVGDocument> document;
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(source, document));
  RendererGeode renderer = createRenderer();
  for (int frame = 0; frame < 3; ++frame) {
    SCOPED_TRACE(frame);
    if (frame == 2) {
      // Stop 18 sits past the 16 the draw parameters carry inline.
      std::optional<SVGElement> stop = document->querySelector("stop:nth-child(19)");
      ASSERT_TRUE(stop.has_value());
      stop->setAttribute("stop-color", "#00ff00");
    }
    renderer.draw(*document);
    EXPECT_THAT(renderer.deviceLost(), testing::IsFalse());
    editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), RenderWithTinySkia(*document),
                                         "many_stop_edit_frame_" + std::to_string(frame),
                                         editor::tests::PixelmatchIdentityParams());
  }
}

/// Gradients drawn inside multi-pass constructs charge the frame's stop budget as the CPU renderer
/// does: a 20-stop gradient fills a rect in a filtered group, in a masked group, and in a pattern
/// tile. Under the default cap and under a cap one stop short of the document's total, Geode's
/// charged stops and rejection verdict equal tiny-skia's.
TEST_F(RendererGeodeTest, GradientStopBudgetInMultiPassConstructsMatchesTinySkia) {
  const std::string stops = GradientStopElements(20);
  const std::string source = GradientTestDocument(
      R"svg(<linearGradient id="g">)svg" + stops +
          R"svg(</linearGradient>)svg"
          R"svg(<filter id="f"><feOffset dx="2" dy="0"/></filter>)svg"
          R"svg(<mask id="m"><rect width="128" height="48" fill="white"/></mask>)svg"
          R"svg(<pattern id="p" patternUnits="userSpaceOnUse" width="16" height="16"><rect width="16" height="16" fill="url(#g)"/></pattern>)svg",
      R"svg(<g filter="url(#f)"><rect width="40" height="48" fill="url(#g)"/></g>)svg"
      R"svg(<g mask="url(#m)"><rect x="44" width="40" height="48" fill="url(#g)"/></g>)svg"
      R"svg(<rect x="88" width="40" height="48" fill="url(#p)"/>)svg");
  const auto stats = [&](bool geode, std::size_t limit) {
    std::optional<SVGDocument> document;
    EXPECT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(source, document));
    if (geode) {
      RendererGeode renderer;
      renderer.setGradientStopBudgetForTesting(limit);
      renderer.draw(*document);
      EXPECT_THAT(renderer.deviceLost(), testing::IsFalse());
      return renderer.resourceStats();
    }
    RendererTinySkia renderer;
    renderer.setGradientStopBudgetForTesting(limit);
    renderer.draw(*document);
    return renderer.resourceStats();
  };
  const RendererResourceStats reference =
      stats(/*geode=*/false, RendererDrawBudget::kMaximumGradientStops);
  ASSERT_FALSE(reference.drawBudgetRejected);
  ASSERT_GT(reference.gradientStops, 0u);
  for (const std::size_t limit :
       {RendererDrawBudget::kMaximumGradientStops, reference.gradientStops - 1u}) {
    SCOPED_TRACE(limit);
    const RendererResourceStats cpu = stats(/*geode=*/false, limit);
    const RendererResourceStats gpu = stats(/*geode=*/true, limit);
    EXPECT_EQ(cpu.drawBudgetRejected, limit < reference.gradientStops)
        << "The lowered cap must reject the frame on the CPU renderer";
    EXPECT_EQ(gpu.gradientStops, cpu.gradientStops);
    EXPECT_EQ(gpu.drawBudgetRejected, cpu.drawBudgetRejected);
  }
}

/// Stop ramps kept across frames are charged, texture and stop key, to the document's resident
/// limit. Six shapes switch one per frame from a solid fill to their own 1300-stop gradient under
/// a limit with room for two ramps: two are kept, the rest draw from a ramp uploaded each frame,
/// and every frame matches tiny-skia.
TEST_F(RendererGeodeTest, KeptStopRampsStayWithinTheDocumentResidentLimit) {
  constexpr int kShapes = 6;
  constexpr int kStops = 1300;
  std::string defs;
  std::string body;
  for (int shape = 0; shape < kShapes; ++shape) {
    defs += "<linearGradient id=\"g" + std::to_string(shape) + "\">";
    for (int i = 0; i < kStops; ++i) {
      char stop[64];
      std::snprintf(stop, sizeof(stop), R"(<stop offset="%.6f" stop-color="#%02x%02x%02x"/>)",
                    i / (kStops - 1.0), i * 200 / (kStops - 1), 200 - i * 200 / (kStops - 1),
                    40 * shape);
      defs += stop;
    }
    defs += "</linearGradient>";
    body += "<rect id=\"r" + std::to_string(shape) + "\" x=\"" + std::to_string(shape * 21) +
            R"svg(" width="20" height="48" fill="#808080"/>)svg";
  }
  const std::string source = GradientTestDocument(defs, body);
  std::optional<SVGDocument> document;
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(source, document));
  RendererGeode renderer = createRenderer();
  renderer.draw(*document);
  auto* budgetSlot =
      document->registry().ctx().find<std::shared_ptr<geode::GeodeDocumentGeometryBudget>>();
  ASSERT_THAT(budgetSlot, testing::NotNull());
  const std::shared_ptr<geode::GeodeDocumentGeometryBudget> budget = *budgetSlot;
  ASSERT_EQ(budget->rampBytes(), 0u);

  // One ramp: 1300 stops lay out as two pairs of 1024-texel rows of 16 bytes, and the stop key
  // holds five floats per stop.
  constexpr std::uint64_t kRampTextureBytes = 1024u * 4u * 16u;
  constexpr std::uint64_t kRampBytes = kRampTextureBytes + kStops * 5u * sizeof(float);
  // Room for the slots' geometry now resident, a margin for the gradient slots' own regions smaller
  // than one ramp, and two ramps.
  const std::uint64_t limit = budget->residentBytes() + 32u * 1024u + 2u * kRampBytes;
  budget->setLimitsForTesting({.cacheBytes = geode::GeodeDocumentGeometryBudget::kMaximumCacheBytes,
                               .residentBytes = limit});

  for (int shape = 0; shape < kShapes; ++shape) {
    SCOPED_TRACE(shape);
    std::optional<SVGElement> rect = document->querySelector("#r" + std::to_string(shape));
    ASSERT_TRUE(rect.has_value());
    rect->setAttribute("fill", "url(#g" + std::to_string(shape) + ")");
    renderer.draw(*document);
    EXPECT_THAT(renderer.deviceLost(), testing::IsFalse());
    EXPECT_LE(budget->residentBytes(), limit);
    EXPECT_EQ(budget->rampBytes(), static_cast<std::uint64_t>(std::min(shape + 1, 2)) * kRampBytes);
    EXPECT_EQ(renderer.resourceStats().gradientRampRetainedBytes, budget->rampBytes());
    editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), RenderWithTinySkia(*document),
                                         "kept_ramps_frame_" + std::to_string(shape),
                                         editor::tests::PixelmatchIdentityParams());
  }

  // An unchanged frame keeps the two kept ramps and uploads the other four again, and still
  // matches tiny-skia.
  renderer.draw(*document);
  EXPECT_EQ(renderer.lastFrameTimings().counters.textureCreates, 4u);
  EXPECT_EQ(renderer.lastFrameTimings().counters.textureWriteBytes, 4u * kRampTextureBytes);
  editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), RenderWithTinySkia(*document),
                                       "kept_ramps_unchanged_frame",
                                       editor::tests::PixelmatchIdentityParams());
}

/// Three independently authored 3x2 PNGs with translucent texels, as data URLs.
constexpr std::array<std::string_view, 3> kImagePngDataUrls = {
    "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAMAAAACCAYAAACddGYaAAAAI0lEQVR42mP438Dwn2HB/"
    "wMaJwIaGE5oLPj//z+DAwMDw38Ari8MarxRuBEAAAAASUVORK5CYII=",
    "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAMAAAACCAYAAACddGYaAAAAIklEQVR42mNgaPj///"
    "8ChgMnNAIaGDROLPjP8P+/A1DoPwC1KA9nEqdknwAAAABJRU5ErkJggg==",
    "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAMAAAACCAYAAACddGYaAAAAI0lEQVR42mOIior6/"
    "0tE5L/IL5H/DCIiv/43MDQsYGhoSAAAmmkLcb1bg0sAAAAASUVORK5CYII=",
};

/// Bytes a 3x2 RGBA8 image texture charges to the document.
constexpr std::uint64_t kImageTextureBytes = 3u * 2u * 4u;

/// A 64x64 document with a background and one `<image>` per entry of \p images, each placed in its
/// own cell and drawn from the PNG at that index of \ref kImagePngDataUrls, or hidden when the
/// entry is negative.
/// @param images Per image, the PNG index to draw or -1 to hide it.
std::string ImagesDocument(std::span<const int> images) {
  std::string svg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">)svg"
                    R"svg(<rect width="64" height="64" fill="#204060"/>)svg";
  for (size_t i = 0; i < images.size(); ++i) {
    const int png = images[i] < 0 ? static_cast<int>(i) : images[i];
    svg += "<image id=\"i" + std::to_string(i) + "\" x=\"" + std::to_string(4 + 20 * i) +
           R"svg(" y="8" width="18" height="12" image-rendering="smooth" href=")svg" +
           std::string(kImagePngDataUrls[static_cast<size_t>(png)]) + "\"" +
           (images[i] < 0 ? R"svg( visibility="hidden")svg" : "") + "/>";
  }
  return svg + "</svg>";
}

/// Renders a fresh parse of \p source with a fresh renderer, a reference that shares no cached
/// state with the renderer under test.
RendererBitmap RenderFreshGeode(const std::shared_ptr<geode::GeodeDevice>& device,
                                const std::string& source) {
  std::optional<SVGDocument> document;
  ParseDocumentWithoutWarnings(source, document);
  if (!document.has_value()) {
    return {};
  }
  RendererGeode reference(device);
  reference.draw(*document);
  return reference.takeSnapshot();
}

/// A warm renderer that keeps an image's texture draws an edited image's new pixels on the next
/// frame: after the image's `href` changes, and after its decoded pixels are rewritten in place,
/// which marks a new revision without an update signal.
TEST_F(RendererGeodeTest, ImageEditsReachTheNextFrameOfAWarmRenderer) {
  const std::array<int, 1> showsA = {0};
  const std::array<int, 1> showsB = {1};
  std::optional<SVGDocument> document;
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(ImagesDocument(showsA), document));
  RendererGeode renderer = createRenderer();
  renderer.draw(*document);
  renderer.draw(*document);
  ASSERT_EQ(renderer.lastFrameTimings().counters.textureCreates, 0u)
      << "Premise: the unchanged frame draws the image from its kept texture";
  const RendererBitmap showingA = renderer.takeSnapshot();

  const RendererBitmap expectedA = RenderFreshGeode(sharedDevice(), ImagesDocument(showsA));
  const RendererBitmap expectedB = RenderFreshGeode(sharedDevice(), ImagesDocument(showsB));
  editor::tests::CompareBitmapToBitmap(showingA, expectedA, "image_edit_before",
                                       editor::tests::PixelmatchIdentityParams());
  editor::tests::BitmapGoldenCompareParams premiseParams =
      editor::tests::PixelmatchIdentityParams();
  premiseParams.maxMismatchedPixels = std::numeric_limits<int>::max();
  int changedPixels = -1;
  editor::tests::CompareBitmapToBitmap(expectedA, expectedB, "image_edit_premise", premiseParams,
                                       &changedPixels);
  ASSERT_THAT(changedPixels, testing::Gt(0)) << "the two images must render differently";

  std::optional<SVGElement> image = document->querySelector("#i0");
  ASSERT_THAT(image, testing::Optional(testing::_));
  image->cast<SVGImageElement>().setHref(kImagePngDataUrls[1]);
  renderer.draw(*document);
  EXPECT_EQ(renderer.lastFrameTimings().counters.textureCreates, 1u)
      << "the new pixels upload once";
  editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), expectedB, "image_edit_href",
                                       editor::tests::PixelmatchIdentityParams());

  // Rewrite the decoded pixels in place with image A's, marking a new revision but sending no
  // update signal.
  std::optional<SVGDocument> documentA;
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(ImagesDocument(showsA), documentA));
  RendererGeode loader = createRenderer();
  loader.draw(*documentA);
  std::optional<SVGElement> imageA = documentA->querySelector("#i0");
  ASSERT_THAT(imageA, testing::Optional(testing::_));
  const auto* loadedA = imageA->unsafeEntityHandle().try_get<components::LoadedImageComponent>();
  ASSERT_THAT(loadedA, testing::NotNull());
  ASSERT_THAT(loadedA->image, testing::Optional(testing::_));
  auto* loaded = image->unsafeEntityHandle().try_get<components::LoadedImageComponent>();
  ASSERT_THAT(loaded, testing::NotNull());
  ASSERT_THAT(loaded->image, testing::Optional(testing::_));
  loaded->image->data = loadedA->image->data;
  loaded->markImageModified();
  renderer.draw(*document);
  EXPECT_EQ(renderer.lastFrameTimings().counters.textureCreates, 1u)
      << "the rewritten pixels upload once";
  editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), expectedA,
                                       "image_edit_pixels_in_place",
                                       editor::tests::PixelmatchIdentityParams());
}

/// Image textures kept across frames are charged to the document's resident limit. Three images
/// appear one per frame under a limit with room for two textures: two are kept, the third uploads
/// a texture for each frame it is drawn in, and every frame matches a fresh rendering.
TEST_F(RendererGeodeTest, KeptImageTexturesStayWithinTheDocumentResidentLimit) {
  std::array<int, 3> shown = {-1, -1, -1};
  std::optional<SVGDocument> document;
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(ImagesDocument(shown), document));
  RendererGeode renderer = createRenderer();
  renderer.draw(*document);
  auto* budgetSlot =
      document->registry().ctx().find<std::shared_ptr<geode::GeodeDocumentGeometryBudget>>();
  ASSERT_THAT(budgetSlot, testing::NotNull());
  const std::shared_ptr<geode::GeodeDocumentGeometryBudget> budget = *budgetSlot;
  ASSERT_EQ(budget->imageBytes(), 0u);

  const std::uint64_t limit = budget->residentBytes() + 2u * kImageTextureBytes;
  budget->setLimitsForTesting({.cacheBytes = geode::GeodeDocumentGeometryBudget::kMaximumCacheBytes,
                               .residentBytes = limit});

  for (int image = 0; image < 3; ++image) {
    SCOPED_TRACE(image);
    shown[static_cast<size_t>(image)] = image;
    std::optional<SVGElement> element = document->querySelector("#i" + std::to_string(image));
    ASSERT_THAT(element, testing::Optional(testing::_));
    element->removeAttribute("visibility");
    renderer.draw(*document);
    EXPECT_LE(budget->residentBytes(), limit);
    EXPECT_EQ(budget->imageBytes(),
              static_cast<std::uint64_t>(std::min(image + 1, 2)) * kImageTextureBytes);
    editor::tests::CompareBitmapToBitmap(
        renderer.takeSnapshot(), RenderFreshGeode(sharedDevice(), ImagesDocument(shown)),
        "kept_images_frame_" + std::to_string(image), editor::tests::PixelmatchIdentityParams());
  }

  // An unchanged frame keeps the two kept textures, uploads the third again, and still matches.
  renderer.draw(*document);
  EXPECT_EQ(renderer.lastFrameTimings().counters.textureCreates, 1u);
  EXPECT_EQ(budget->imageBytes(), 2u * kImageTextureBytes);
  editor::tests::CompareBitmapToBitmap(
      renderer.takeSnapshot(), RenderFreshGeode(sharedDevice(), ImagesDocument(shown)),
      "kept_images_unchanged_frame", editor::tests::PixelmatchIdentityParams());
}

/// A document with one image element and \p repeats `<use>` references to it, each drawing the
/// image at its own position.
/// @param repeats Number of `<use>` references.
std::string RepeatedImageDocument(size_t repeats) {
  std::string svg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">)svg"
      R"svg(<rect width="64" height="64" fill="#204060"/><defs>)svg"
      R"svg(<image id="img" width="6" height="4" image-rendering="smooth" href=")svg" +
      std::string(kImagePngDataUrls[0]) + R"svg("/></defs>)svg";
  for (size_t i = 0; i < repeats; ++i) {
    svg += "<use href=\"#img\" x=\"" + std::to_string(i % 10 * 6) + "\" y=\"" +
           std::to_string(i / 10 % 16 * 4) + "\"/>";
  }
  return svg + "</svg>";
}

/// Matches retained entry counts with \p images image blit entries, whatever the composite counts.
/// @param images Expected image blit entries.
testing::Matcher<RendererGeode::RetainedCompositeCounts> ImageBlitEntries(size_t images) {
  return testing::Field("images", &RendererGeode::RetainedCompositeCounts::images, images);
}

/// A renderer keeps a blit entry for each kept image texture its last frame drew, at most
/// \ref RendererGeode::maxRetainedCompositesForTesting: blits past that draw with a uniform and
/// bind group of their own frame, entries the last frame did not reach are dropped when it ends,
/// and trimming the texture pool to nothing releases them all. Every frame matches a fresh
/// rendering.
TEST_F(RendererGeodeTest, ImageBlitEntriesStayCappedAndFollowTheLastFrame) {
  const size_t cap = RendererGeode::maxRetainedCompositesForTesting();
  const std::string manySource = RepeatedImageDocument(cap + 3);
  const std::string fewSource = RepeatedImageDocument(2);
  std::optional<SVGDocument> many;
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(manySource, many));
  std::optional<SVGDocument> few;
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(fewSource, few));

  RendererGeode renderer = createRenderer();
  for (int frame = 1; frame <= 2; ++frame) {
    SCOPED_TRACE(testing::Message() << "frame " << frame);
    renderer.draw(*many);
    EXPECT_THAT(renderer.retainedCompositeCountsForTesting(), ImageBlitEntries(cap));
    if (frame == 2) {
      EXPECT_EQ(renderer.lastFrameTimings().counters.textureCreates, 0u);
      EXPECT_GT(renderer.lastFrameTimings().counters.bufferWrites, 0u)
          << "the blits past the cap write their uniforms for this frame";
    }
    editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(),
                                         RenderFreshGeode(sharedDevice(), manySource),
                                         "image_blits_past_the_cap_frame_" + std::to_string(frame),
                                         editor::tests::PixelmatchIdentityParams());
  }

  renderer.draw(*few);
  EXPECT_THAT(renderer.retainedCompositeCountsForTesting(), ImageBlitEntries(2))
      << "entries the last frame did not reach must be dropped when it ends";
  renderer.draw(*few);
  EXPECT_EQ(renderer.lastFrameTimings().counters.bufferWrites, 0u);
  EXPECT_EQ(renderer.lastFrameTimings().counters.bindgroupCreates, 0u);

  renderer.trimTexturePool(RendererGeodeTexturePoolTrim::All);
  EXPECT_THAT(renderer.retainedCompositeCountsForTesting(), ImageBlitEntries(0));
  renderer.draw(*few);
  EXPECT_THAT(renderer.retainedCompositeCountsForTesting(), ImageBlitEntries(2));
  editor::tests::CompareBitmapToBitmap(
      renderer.takeSnapshot(), RenderFreshGeode(sharedDevice(), fewSource),
      "image_blit_entries_after_trim", editor::tests::PixelmatchIdentityParams());
}

/// Edits that change only what a kept image's blit draws with, its placement and its sampling,
/// reach a warm renderer's next frame: each matches a fresh rendering of the edited document.
TEST_F(RendererGeodeTest, ImageBlitEditsMatchAFreshRenderer) {
  struct Edit {
    const char* attribute;
    const char* value;
  };
  constexpr std::array<Edit, 4> kEdits = {{{"x", "9"},
                                           {"width", "30"},
                                           {"image-rendering", "pixelated"},
                                           {"transform", "rotate(10 20 20)"}}};
  const std::array<int, 1> showsA = {0};
  std::optional<SVGDocument> document;
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(ImagesDocument(showsA), document));
  std::optional<SVGElement> image = document->querySelector("#i0");
  ASSERT_THAT(image, testing::Optional(testing::_));
  RendererGeode renderer = createRenderer();
  renderer.draw(*document);
  renderer.draw(*document);

  editor::tests::BitmapGoldenCompareParams premiseParams =
      editor::tests::PixelmatchIdentityParams();
  premiseParams.maxMismatchedPixels = std::numeric_limits<int>::max();
  for (const Edit& edit : kEdits) {
    SCOPED_TRACE(edit.attribute);
    const RendererBitmap before = renderer.takeSnapshot();
    image->setAttribute(edit.attribute, edit.value);
    renderer.draw(*document);
    RendererGeode reference = createRenderer();
    reference.draw(*document);
    const RendererBitmap expected = reference.takeSnapshot();
    int changedPixels = -1;
    editor::tests::CompareBitmapToBitmap(before, expected,
                                         std::string("image_blit_edit_premise_") + edit.attribute,
                                         premiseParams, &changedPixels);
    ASSERT_THAT(changedPixels, testing::Gt(0)) << "the edit must change the rendering";
    editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), expected,
                                         std::string("image_blit_edit_") + edit.attribute,
                                         editor::tests::PixelmatchIdentityParams());
  }
}

/// A kept image texture can be destroyed while renderers other than the one drawing still hold
/// blit entries built from it: its pixels change or go away, a resident limit refuses the new
/// pixels, or its document is destroyed. The device's handle retirement reports it to every
/// renderer on the device before letting go of it, so no entry keeps a view of a destroyed
/// texture, and the next frame rebuilds from live textures.
TEST_F(RendererGeodeTest, ImageBlitsReleaseViewsBeforeTheirTexturesAreDestroyed) {
  const std::array<int, 1> showsA = {0};
  const std::array<int, 1> showsB = {1};
  // A device of its own, so only these renderers hold entries and only this test retires textures.
  const std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
  ASSERT_THAT(device, testing::NotNull());
  RendererGeode drawing(device);
  RendererGeode holding(device);

  struct Path {
    const char* name;
    std::function<void(SVGDocument&, SVGElement&)> destroy;
  };
  const std::array<Path, 3> kPaths = {{
      {"new_pixels",
       [](SVGDocument&, SVGElement& image) {
         image.cast<SVGImageElement>().setHref(kImagePngDataUrls[1]);
       }},
      {"pixels_dropped",
       [](SVGDocument&, SVGElement& image) { image.cast<SVGImageElement>().setHref(""); }},
      {"new_pixels_refused",
       [](SVGDocument& document, SVGElement& image) {
         auto* budget =
             document.registry().ctx().find<std::shared_ptr<geode::GeodeDocumentGeometryBudget>>();
         ASSERT_THAT(budget, testing::NotNull());
         (*budget)->setLimitsForTesting(
             {.cacheBytes = geode::GeodeDocumentGeometryBudget::kMaximumCacheBytes,
              .residentBytes = 0});
         image.cast<SVGImageElement>().setHref(kImagePngDataUrls[1]);
       }},
  }};
  for (const Path& path : kPaths) {
    SCOPED_TRACE(path.name);
    std::optional<SVGDocument> document;
    ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(ImagesDocument(showsA), document));
    drawing.draw(*document);
    holding.draw(*document);
    ASSERT_THAT(holding.retainedCompositeViewHoldersForTesting(), ImageBlitEntries(1));
    std::optional<SVGElement> image = document->querySelector("#i0");
    ASSERT_THAT(image, testing::Optional(testing::_));
    ASSERT_NO_FATAL_FAILURE(path.destroy(*document, *image));

    // The drawing renderer's frame retires the old texture and releases it at its end.
    drawing.draw(*document);
    EXPECT_THAT(holding.retainedCompositeViewHoldersForTesting(), ImageBlitEntries(0))
        << "an entry of another renderer kept a view of a destroyed image texture";

    holding.draw(*document);
    EXPECT_THAT(holding.deviceLost(), testing::IsFalse());
    editor::tests::CompareBitmapToBitmap(
        holding.takeSnapshot(),
        RenderFreshGeode(device, ImagesDocument(std::string_view(path.name) == "pixels_dropped"
                                                    ? std::span<const int>()
                                                    : std::span<const int>(showsB))),
        std::string("image_blit_views_after_") + path.name,
        editor::tests::PixelmatchIdentityParams());
  }

  // Document teardown, outside any frame: its textures are released at the next frame's end on
  // the device, whichever document that frame draws.
  {
    std::optional<SVGDocument> document;
    ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(ImagesDocument(showsA), document));
    drawing.draw(*document);
    holding.draw(*document);
    ASSERT_THAT(holding.retainedCompositeViewHoldersForTesting(), ImageBlitEntries(1));
  }
  std::optional<SVGDocument> other;
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(ImagesDocument(showsB), other));
  drawing.draw(*other);
  EXPECT_THAT(holding.retainedCompositeViewHoldersForTesting(), ImageBlitEntries(0))
      << "an entry kept a view of a texture of a destroyed document";
}

/// A frame that ends without submitting releases the views and bind group of every image blit
/// entry, like the composite entries, and the next frame rebuilds them.
TEST_F(RendererGeodeTest, ImageBlitsReleaseViewsWhenAFrameIsAbandoned) {
  const std::array<int, 1> showsA = {0};
  std::optional<SVGDocument> document;
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(ImagesDocument(showsA), document));
  const std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
  ASSERT_THAT(device, testing::NotNull());
  RendererGeode renderer(device);
  std::optional<SVGElement> image = document->querySelector("#i0");
  ASSERT_THAT(image, testing::Optional(testing::_));
  const EntityHandle source = image->unsafeEntityHandle();
  const auto drawImage = [&] {
    const auto* loaded = source.try_get<components::LoadedImageComponent>();
    ASSERT_THAT(loaded, testing::NotNull());
    ASSERT_THAT(loaded->image, testing::Optional(testing::_));
    renderer.drawImage(*loaded->image,
                       ImageParams{.targetRect = Box2d({4, 8}, {22, 20}), .sourceEntity = source});
  };

  renderer.draw(*document);
  ASSERT_THAT(renderer.retainedCompositeViewHoldersForTesting(), ImageBlitEntries(1));
  beginFrame(renderer);
  ASSERT_NO_FATAL_FAILURE(drawImage());
  beginFrame(renderer);
  EXPECT_THAT(renderer.retainedCompositeViewHoldersForTesting(), ImageBlitEntries(0))
      << "a frame that never reached endFrame must release every image entry's views";
  renderer.endFrame();

  renderer.draw(*document);
  ASSERT_THAT(renderer.retainedCompositeViewHoldersForTesting(), ImageBlitEntries(1));
  beginFrame(renderer);
  ASSERT_NO_FATAL_FAILURE(drawImage());
  renderer.injectActiveArenaWriteFailureForTesting();
  renderer.setPaint(solidFill(css::RGBA(0, 0, 255, 255)));
  renderer.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});
  renderer.endFrame();
  EXPECT_THAT(renderer.deviceLost(), testing::IsFalse());
  EXPECT_THAT(renderer.retainedCompositeViewHoldersForTesting(), ImageBlitEntries(0))
      << "a frame abandoned at endFrame must release every image entry's views";

  ASSERT_THAT(device->waitForQueueIdle(), testing::Eq(geode::GpuWaitResult::Complete));
  renderer.draw(*document);
  EXPECT_THAT(renderer.deviceLost(), testing::IsFalse());
  EXPECT_THAT(renderer.retainedCompositeViewHoldersForTesting(), ImageBlitEntries(1));
  editor::tests::CompareBitmapToBitmap(
      renderer.takeSnapshot(), RenderFreshGeode(device, ImagesDocument(showsA)),
      "image_blits_after_abandoned_frames", editor::tests::PixelmatchIdentityParams());
}

/// An image under a path clip keeps the bind group naming the clip mask's retained view across
/// unchanged frames, so those frames write and create nothing for it. The image's own texture is
/// kept outside the texture pool, so a pool trim by another renderer on the device destroys only
/// the clip mask's texture: the entry stays, but must release the bind group naming that texture's
/// view, and the next frame rebuilds it and matches a fresh rendering.
TEST_F(RendererGeodeTest, ClippedImageBlitsKeepTheirClipMaskViewUntilItGoes) {
  const std::string source =
      std::string(
          R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">)svg"
          R"svg(<clipPath id="tri"><path d="M2 2 L60 6 L10 60 Z"/></clipPath>)svg"
          R"svg(<image clip-path="url(#tri)" x="4" y="4" width="54" height="36" href=")svg") +
      std::string(kImagePngDataUrls[2]) + R"svg("/></svg>)svg";
  std::optional<SVGDocument> document;
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(source, document));
  // A device of its own, so the pool holds only what these renderers put there. A renderer's own
  // trim also releases its retained entries, so the trim comes from another renderer.
  const std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
  ASSERT_THAT(device, testing::NotNull());
  RendererGeode renderer(device);
  RendererGeode other(device);
  const auto drawAndCompare = [&](const std::string& label) {
    renderer.draw(*document);
    EXPECT_THAT(renderer.deviceLost(), testing::IsFalse()) << label;
    EXPECT_THAT(renderer.retainedCompositeCountsForTesting(), ImageBlitEntries(1))
        << "Premise: the clipped image draws through a retained entry";
    EXPECT_THAT(renderer.retainedCompositeViewHoldersForTesting(), ImageBlitEntries(1))
        << label << ": the entry keeps the bind group naming the clip mask's view";
    editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(),
                                         RenderFreshGeode(sharedDevice(), source), label,
                                         editor::tests::PixelmatchIdentityParams());
  };
  for (int frame = 1; frame <= 3; ++frame) {
    SCOPED_TRACE(testing::Message() << "frame " << frame);
    drawAndCompare("clipped_image_blit_frame_" + std::to_string(frame));
    if (frame > 1) {
      EXPECT_EQ(renderer.lastFrameTimings().counters.bindgroupCreates, 0u);
      EXPECT_EQ(renderer.lastFrameTimings().counters.bufferWrites, 0u);
    }
  }

  other.trimTexturePool(RendererGeodeTexturePoolTrim::All);
  EXPECT_THAT(renderer.retainedCompositeCountsForTesting(), ImageBlitEntries(1))
      << "Premise: another renderer's trim leaves this renderer's entry in place";
  EXPECT_THAT(renderer.retainedCompositeViewHoldersForTesting(), ImageBlitEntries(0))
      << "the trim destroyed the clip mask's texture, so the entry must let go of the bind group "
         "naming its view";
  drawAndCompare("clipped_image_blit_after_clip_texture_destroyed");
}

/// A render snapshot carries a copy of each image's pixels and no handle into the document, so
/// replaying it draws every image from a texture uploaded for that frame: the document gains no
/// resident image component and no budget, and the snapshot holds no live reference to it.
TEST_F(RendererGeodeTest, ReplayedImageSnapshotLeavesTheDocumentUntouched) {
  const std::array<int, 3> shown = {0, 1, 2};
  std::optional<SVGDocument> document;
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(ImagesDocument(shown), document));
  RendererGeode renderer = createRenderer();
  RendererDriver driver(renderer);
  const RenderSnapshot snapshot = driver.captureRenderSnapshot(*document);
  EXPECT_THAT(snapshot.liveRegistryReferenceCountForTesting(document->registry()), testing::Eq(0u));

  for (int frame = 1; frame <= 2; ++frame) {
    SCOPED_TRACE(testing::Message() << "replay " << frame);
    driver.draw(snapshot);
    EXPECT_THAT(renderer.lastFrameTimings().counters.textureCreates, testing::Ge(3u))
        << "a replayed image uploads a texture for its frame";
    EXPECT_THAT(document->registry().view<geode::GeodeResidentImageComponent>().size(),
                testing::Eq(0u))
        << "replay must not install a resident image component on the live document";
    EXPECT_THAT(
        document->registry().ctx().find<std::shared_ptr<geode::GeodeDocumentGeometryBudget>>(),
        testing::IsNull())
        << "replay must not create or charge the live document's budget";
    EXPECT_THAT(renderer.retainedCompositeCountsForTesting(), ImageBlitEntries(0));
    editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(),
                                         RenderFreshGeode(sharedDevice(), ImagesDocument(shown)),
                                         "replayed_image_snapshot_" + std::to_string(frame),
                                         editor::tests::PixelmatchIdentityParams());
  }
}

/// An open frame that drew an image from its kept texture still samples that texture when, before
/// the frame ends, another renderer on the device draws the document after the image's pixels
/// changed.
TEST_F(RendererGeodeTest, NestedPassKeepsTheImageTextureAnOuterFrameLeftPending) {
  const std::array<int, 1> showsA = {0};
  std::optional<SVGDocument> document;
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(ImagesDocument(showsA), document));
  RendererGeode warm = createRenderer();
  warm.draw(*document);
  std::optional<SVGElement> image = document->querySelector("#i0");
  ASSERT_THAT(image, testing::Optional(testing::_));
  const EntityHandle source = image->unsafeEntityHandle();
  const auto* loaded = source.try_get<components::LoadedImageComponent>();
  ASSERT_THAT(loaded, testing::NotNull());
  ASSERT_THAT(loaded->image, testing::Optional(testing::_));
  const ImageParams params{.targetRect = Box2d({8, 8}, {56, 40}), .sourceEntity = source};

  RendererGeode reference = createRenderer();
  beginFrame(reference);
  reference.drawImage(*loaded->image, ImageParams{.targetRect = params.targetRect});
  reference.endFrame();
  const RendererBitmap expected = reference.takeSnapshot();
  ASSERT_THAT(expected.empty(), testing::IsFalse());

  RendererGeode outer = createRenderer();
  beginFrame(outer);
  outer.drawImage(*loaded->image, params);

  // The image now holds other pixels; the document is drawn while the outer frame is open.
  image->cast<SVGImageElement>().setHref(kImagePngDataUrls[1]);
  warm.draw(*document);
  auto* budget =
      document->registry().ctx().find<std::shared_ptr<geode::GeodeDocumentGeometryBudget>>();
  ASSERT_THAT(budget, testing::NotNull());
  EXPECT_THAT((*budget)->imageBytes(), testing::Eq(2u * kImageTextureBytes))
      << "the texture the open outer frame samples must stay held, and charged, until no open "
         "frame can read it; it was let go of when the image changed";

  outer.endFrame();
  editor::tests::CompareBitmapToBitmap(outer.takeSnapshot(), expected,
                                       "nested_pass_keeps_pending_image_texture",
                                       editor::tests::PixelmatchIdentityParams());
}

/// A draw the frame's gradient-stop budget refuses keeps every draw before it, whatever queued
/// it, and drops every draw after it, as on the CPU renderer. Each case renders a document whose
/// refused gradient, 20 stops or a batchable 10, sits after `before` and before `after`, under a
/// cap that admits the stops of `before` only. The Geode frame must equal Geode's unbounded
/// rendering of `before` alone, and the charged stops, verdict and pixels must equal tiny-skia's.
/// Text before a refusal is covered in GeodeGlyphInstancing_tests, whose binary has the text
/// backend this one is built without.
TEST_F(RendererGeodeTest, DrawsBeforeAGradientStopRefusalAreKept) {
  const std::string twentyStops = GradientStopElements(20);
  const std::string defs =
      R"svg(<linearGradient id="big">)svg" + twentyStops +
      R"svg(</linearGradient><linearGradient id="small"><stop stop-color="red"/>)svg"
      R"svg(<stop offset="1" stop-color="blue"/></linearGradient>)svg"
      R"svg(<linearGradient id="ten"><stop stop-color="red"/><stop offset="0.1" stop-color="lime"/>)svg"
      R"svg(<stop offset="0.2" stop-color="blue"/><stop offset="0.3" stop-color="yellow"/>)svg"
      R"svg(<stop offset="0.4" stop-color="cyan"/><stop offset="0.5" stop-color="magenta"/>)svg"
      R"svg(<stop offset="0.6" stop-color="black"/><stop offset="0.7" stop-color="white"/>)svg"
      R"svg(<stop offset="0.8" stop-color="gray"/><stop offset="1" stop-color="orange"/>)svg"
      R"svg(</linearGradient>)svg";
  // A 20-stop gradient is refused on the ordinary emit path; a 10-stop one, which can batch, is
  // refused while the batch resolves it, before any pending draw is flushed.
  constexpr std::string_view kRefusedSmall =
      R"svg(<rect x="64" width="32" height="48" fill="url(#ten)"/>)svg";
  constexpr std::string_view kRefused =
      R"svg(<rect x="64" width="32" height="48" fill="url(#big)"/>)svg";
  constexpr std::string_view kAfter =
      R"svg(<rect x="100" width="28" height="48" fill="#208020"/>)svg";
  struct OrderCase {
    std::string_view label;
    std::string_view before;
    std::size_t cap;
    bool batchableRefused = false;
  };
  // The `batched_gradient_before*` and `stroke_before` cases are parity rows rather than
  // regression rows: nothing here asserts their draws were queued in a batch before the refusal,
  // and they passed before the queued draws were kept as well.
  const std::array<OrderCase, 7> cases = {{
      {"solid_fill_before_batchable_refusal",
       R"svg(<rect width="60" height="48" fill="#d02020"/>)svg", 9u, true},
      {"batched_gradient_before_batchable_refusal",
       R"svg(<rect width="30" height="48" fill="url(#small)"/><rect x="30" width="30" height="48" fill="#d02020"/>)svg",
       11u, true},
      {"solid_fill_before", R"svg(<rect width="60" height="48" fill="#d02020"/>)svg", 19u},
      {"batched_gradient_before",
       R"svg(<rect width="30" height="48" fill="url(#small)"/><rect x="30" width="30" height="48" fill="#d02020"/>)svg",
       21u},
      {"stroke_before",
       R"svg(<path d="M4,24 H56" stroke="#d02020" stroke-width="16" fill="none"/>)svg", 19u},
      {"layer_before",
       R"svg(<g opacity="0.5"><rect width="60" height="48" fill="#d02020"/></g>)svg", 19u},
      {"refused_first", "", 19u},
  }};
  for (const OrderCase& orderCase : cases) {
    SCOPED_TRACE(orderCase.label);
    const std::string source = GradientTestDocument(
        defs, std::string(orderCase.before) +
                  std::string(orderCase.batchableRefused ? kRefusedSmall : kRefused) +
                  std::string(kAfter));
    const std::string prefix = GradientTestDocument(defs, orderCase.before);

    std::optional<SVGDocument> document;
    std::optional<SVGDocument> referenceDocument;
    std::optional<SVGDocument> prefixDocument;
    ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(source, document));
    ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(source, referenceDocument));
    ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(prefix, prefixDocument));

    RendererTinySkia reference;
    reference.setGradientStopBudgetForTesting(orderCase.cap);
    reference.draw(*referenceDocument);
    RendererGeode renderer;
    renderer.setGradientStopBudgetForTesting(orderCase.cap);
    renderer.draw(*document);
    EXPECT_THAT(renderer.deviceLost(), testing::IsFalse());
    const RendererResourceStats stats = renderer.resourceStats();
    const RendererResourceStats referenceStats = reference.resourceStats();
    EXPECT_TRUE(referenceStats.drawBudgetRejected);
    EXPECT_EQ(stats.drawBudgetRejected, referenceStats.drawBudgetRejected);
    EXPECT_EQ(stats.gradientStops, referenceStats.gradientStops);

    const RendererBitmap actual = renderer.takeSnapshot();
    RendererGeode unbounded;
    unbounded.draw(*prefixDocument);
    editor::tests::CompareBitmapToBitmap(actual, unbounded.takeSnapshot(),
                                         "stop_refusal_keeps_" + std::string(orderCase.label),
                                         editor::tests::PixelmatchIdentityParams());
    editor::tests::CompareBitmapToBitmap(actual, reference.takeSnapshot(),
                                         "stop_refusal_matches_cpu_" + std::string(orderCase.label),
                                         editor::tests::PixelmatchIdentityParams());
  }
}

/// A draw the frame's geometry budget refuses keeps the draws before it too: a solid rect, then a
/// path with more encoded items than the lowered frame limit leaves, then another rect. The frame
/// must equal the unbounded rendering of the first rect alone.
TEST_F(RendererGeodeTest, DrawsBeforeAGeometryRefusalAreKept) {
  std::string wave = "M0,24";
  for (int i = 1; i <= 120; ++i) {
    wave += " Q" + std::to_string(i - 0.5) + "," + std::to_string(i % 2 == 0 ? 4 : 44) + " " +
            std::to_string(i) + ",24";
  }
  const std::string before = R"svg(<rect width="60" height="48" fill="#d02020"/>)svg";
  const std::string source = GradientTestDocument(
      "",
      before + R"svg(<path d=")svg" + wave +
          R"svg( V48 H0 Z" fill="#2020d0"/><rect x="100" width="28" height="48" fill="#208020"/>)svg");
  std::optional<SVGDocument> document;
  std::optional<SVGDocument> prefixDocument;
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(source, document));
  ASSERT_NO_FATAL_FAILURE(
      ParseDocumentWithoutWarnings(GradientTestDocument("", before), prefixDocument));
  RendererGeode renderer;
  renderer.setGeometryBudgetForTesting(geode::GeodeFrameGeometryBudget::kMaximumDraws, 64u,
                                       geode::GeodeFrameGeometryBudget::kMaximumRetainedBytes,
                                       geode::GeodeDocumentGeometryBudget::kMaximumCacheBytes,
                                       geode::GeodeDocumentGeometryBudget::kMaximumResidentBytes);
  renderer.draw(*document);
  EXPECT_THAT(renderer.deviceLost(), testing::IsFalse());
  EXPECT_TRUE(renderer.resourceStats().geometryBudgetRejected) << "Premise: the wave is refused";
  RendererGeode unbounded;
  unbounded.draw(*prefixDocument);
  editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), unbounded.takeSnapshot(),
                                       "geometry_refusal_keeps_earlier_draw",
                                       editor::tests::PixelmatchIdentityParams());
}

/// Every surface the renderer opens while an expanded filter layer is active has the layer's
/// extent: a group opacity layer, a mix-blend-mode layer and its backdrop, a mask and its content,
/// a nested filter, and a clip mask, including one opened while drawing a mask's content.
/// Otherwise content drawn into the overhang or along the far edges of the layer is lost, and what
/// remains is rescaled when the surface composites back. Content under a nested filter also keeps
/// the expanded layer's offset, and under a nested filter that expands too it keeps the sum of
/// both offsets, so it lands where the rest does. The content reaches past every edge of the
/// surface, with a small square in the overhang and another at the far corner of the layer.
TEST_F(RendererGeodeTest, SurfacesInsideAnExpandedFilterLayerMatchTinySkia) {
  struct SurfaceCase {
    std::string_view label;
    std::string_view defs;
    /// Drawn inside the filtered group, before the group the case is about.
    std::string_view backdrop;
    std::string_view groupAttributes;
    /// Attributes of the rect that covers the whole filter region.
    std::string_view coverAttributes;
    editor::tests::BitmapGoldenCompareParams params = editor::tests::PixelmatchIdentityParams();
  };
  constexpr std::array<SurfaceCase, 8> kCases = {{
      {.label = "expanded_filter_opacity_layer", .groupAttributes = R"svg(opacity="0.5")svg"},
      {.label = "expanded_filter_blend_layer",
       .backdrop = R"svg(<rect x="-8" y="-8" width="80" height="40" fill="#e0c020"/>)svg",
       .groupAttributes = R"svg(style="mix-blend-mode:multiply")svg"},
      {.label = "expanded_filter_mask",
       .defs =
           R"svg(<mask id="m" maskUnits="userSpaceOnUse" x="-8" y="-8" width="80" height="80"><rect x="-8" y="-8" width="80" height="80" fill="white"/><rect x="20" y="-8" width="8" height="80" fill="black"/></mask>)svg",
       .groupAttributes = R"svg(mask="url(#m)")svg"},
      {.label = "expanded_filter_nested_filter",
       .defs =
           R"svg(<filter id="fade" filterUnits="userSpaceOnUse" x="-8" y="-8" width="80" height="80"><feComponentTransfer><feFuncA type="linear" slope="0.5"/></feComponentTransfer></filter>)svg",
       .groupAttributes = R"svg(filter="url(#fade)")svg"},
      // The nested region reaches past the outer one on every side, so it starts above and left of
      // the outer capture and expands it a second time.
      {.label = "expanded_filter_nested_expanded_filter",
       .defs =
           R"svg(<filter id="drift" filterUnits="userSpaceOnUse" x="-16" y="-16" width="96" height="96"><feOffset dx="2" dy="2"/></filter>)svg",
       .groupAttributes = R"svg(filter="url(#drift)")svg"},
      {.label = "expanded_filter_clip_path",
       .defs = R"svg(<clipPath id="c"><circle cx="32" cy="32" r="34"/></clipPath>)svg",
       .groupAttributes = R"svg(clip-path="url(#c)")svg",
       .params = kCircleEdgeParams},
      {.label = "expanded_filter_clip_path_in_opacity_layer",
       .defs = R"svg(<clipPath id="c"><circle cx="32" cy="32" r="34"/></clipPath>)svg",
       .groupAttributes = R"svg(opacity="0.5" clip-path="url(#c)")svg",
       .params = kCircleEdgeParams},
      {.label = "expanded_filter_clip_path_in_mask",
       .defs =
           R"svg(<mask id="m" maskUnits="userSpaceOnUse" x="-8" y="-8" width="80" height="80"><rect x="-8" y="-8" width="80" height="80" fill="white"/><rect x="20" y="-8" width="8" height="80" fill="black"/></mask><clipPath id="c"><circle cx="32" cy="32" r="34"/></clipPath>)svg",
       .groupAttributes = R"svg(mask="url(#m)")svg",
       .coverAttributes = R"svg(clip-path="url(#c)")svg",
       .params = kCircleEdgeParams},
  }};
  const auto document = [](const SurfaceCase& surfaceCase) {
    std::string source =
        R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64"><defs><filter id="shift" filterUnits="userSpaceOnUse" x="-8" y="-8" width="80" height="80"><feOffset dx="4" dy="4"/></filter>)svg";
    source += surfaceCase.defs;
    source += "</defs>";
    source += R"svg(<g filter="url(#shift)">)svg";
    source += surfaceCase.backdrop;
    source += "<g ";
    source += surfaceCase.groupAttributes;
    source += R"svg(><rect x="-8" y="-8" width="80" height="80" fill="#2050c0" )svg";
    source += surfaceCase.coverAttributes;
    source +=
        R"svg(/><rect x="-6" y="-6" width="10" height="10" fill="#20c040"/><rect x="54" y="54" width="10" height="10" fill="#c02020"/></g></g></svg>)svg";
    return source;
  };
  for (const SurfaceCase& surfaceCase : kCases) {
    SCOPED_TRACE(surfaceCase.label);
    const std::string source = document(surfaceCase);
    ExpectGeodeMatchesTinySkia(source, source, surfaceCase.label, surfaceCase.params);
  }
}

/// The Layers panel thumbnail of a group with an feOffset filter and a clipped child. The
/// thumbnail is cropped to the group's geometry, so the default filter region, which reaches ten
/// percent past that geometry, starts above and left of the thumbnail and the filter expands.
TEST_F(RendererGeodeTest, ThumbnailOfAnOffsetFilteredGroupWithAClippedChildMatchesTinySkia) {
  std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
  ASSERT_THAT(device, testing::NotNull());
  std::optional<SVGDocument> document;
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200"><clipPath id="c"><circle cx="100" cy="100" r="40"/></clipPath><filter id="f"><feOffset dx="4" dy="4"/></filter><g filter="url(#f)"><rect x="50" y="50" width="100" height="100" fill="blue" clip-path="url(#c)"/></g></svg>)svg",
      document));
  // The group's geometry spans (50, 50) to (150, 150), which fits a 42x24 cell at a scale of 0.24
  // as a 24x24 image: the same view as a 24x24 document with that box as its viewBox.
  std::optional<SVGDocument> reference;
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="50 50 100 100"><clipPath id="c"><circle cx="100" cy="100" r="40"/></clipPath><filter id="f"><feOffset dx="4" dy="4"/></filter><g filter="url(#f)"><rect x="50" y="50" width="100" height="100" fill="blue" clip-path="url(#c)"/></g></svg>)svg",
      reference));
  const std::optional<SVGElement> group = document->querySelector("g");
  ASSERT_THAT(group, testing::Optional(testing::_));

  Renderer renderer(device);
  const RendererImage thumbnail = renderer.renderElement(*group, Vector2i(42, 24));
  EXPECT_THAT(device->isDeviceLost(), testing::IsFalse());
  editor::tests::CompareBitmapToBitmap(thumbnail.bitmap(), RenderWithTinySkia(*reference),
                                       "offset_filter_clipped_child_thumbnail", kCircleEdgeParams);
}

/// feColorMatrix type=luminanceToAlpha: red → alpha based on Y-channel luminance.
TEST_F(RendererGeodeTest, FilterColorMatrixLuminanceToAlpha) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  components::FilterGraph graph;
  components::FilterNode cmNode;
  components::filter_primitive::ColorMatrix cm;
  cm.type = components::filter_primitive::ColorMatrix::Type::LuminanceToAlpha;
  cmNode.primitive = cm;
  cmNode.inputs.push_back(components::FilterStandardInput::SourceGraphic);
  graph.nodes.push_back(cmNode);

  renderer.pushFilterLayer(graph, Box2d({0, 0}, {kViewportSize, kViewportSize}));

  // Pure red: luminance contribution from R is 0.2126.
  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  renderer.setTransform(Transform2d());
  renderer.drawRect(Box2d({16, 16}, {48, 48}), StrokeParams{});

  renderer.popFilterLayer();
  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  ASSERT_FALSE(snap.empty());

  // luminanceToAlpha: R'=0, G'=0, B'=0, A'= 0.2126*R + 0.7152*G + 0.0722*B.
  // For pure red (R=1.0): A' = 0.2126 → ~54 in [0, 255].
  auto center = PixelAt(snap, 32, 32);
  EXPECT_THAT(center, Rgba(testing::Eq(0), testing::Eq(0), testing::Eq(0), Near(54, 3)))
      << "Alpha should match Y-channel luminance of red";

  // Outside the rect: transparent.
  auto outside = PixelAt(snap, 4, 4);
  EXPECT_THAT(outside, IsTransparent()) << "Outside should be transparent";
}

TEST_F(RendererGeodeTest, FilterSourceAlphaInputExtractsAlphaChannel) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  components::FilterGraph graph;
  components::FilterNode offsetNode;
  components::filter_primitive::Offset offset;
  offset.dx = 0.0;
  offset.dy = 0.0;
  offsetNode.primitive = offset;
  offsetNode.inputs.push_back(components::FilterStandardInput::SourceAlpha);
  graph.nodes.push_back(offsetNode);

  renderer.pushFilterLayer(graph, Box2d({0, 0}, {kViewportSize, kViewportSize}));

  renderer.setPaint(solidFill(css::RGBA(200, 100, 50, 255)));
  renderer.setTransform(Transform2d());
  renderer.drawRect(Box2d({16, 16}, {48, 48}), StrokeParams{});

  renderer.popFilterLayer();
  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  ASSERT_FALSE(snap.empty());

  const auto center = PixelAt(snap, 32, 32);
  EXPECT_THAT(center, RgbaEq(0, 0, 0, 255));
}

// Per SVG spec, feSpecularLighting `specularExponent` must be in [1, 128]; a value
// < 1 produces transparent-black output (the same rule tiny-skia applies in its
// filter pipeline - see FilterGraph.cpp's `specularExponent >= 1.0` guard).
// GeodeFilterEngine clamps/short-circuits to match; this pins the spec behavior.
TEST_F(RendererGeodeTest, FilterSpecularLightingExponentBelowOneIsTransparent) {
  components::FilterGraph graph;
  components::FilterNode specularNode;
  components::filter_primitive::SpecularLighting specular;
  specular.surfaceScale = 3.0;
  specular.specularConstant = 1.0;
  specular.specularExponent = 0.0;  // < 1 → transparent per spec.
  specular.lightingColor = css::Color(css::RGBA(255, 255, 255, 255));

  components::filter_primitive::LightSource light;
  light.type = components::filter_primitive::LightSource::Type::Point;
  light.x = 48.0;
  light.y = 16.0;
  light.z = 24.0;
  specular.light = light;

  specularNode.primitive = specular;
  specularNode.inputs.push_back(components::FilterStandardInput::SourceGraphic);
  graph.nodes.push_back(specularNode);

  const Transform2d deviceFromFilter = Transform2d();
  const RendererBitmap actual = renderFlatSourceThroughFilter(graph, deviceFromFilter);
  ASSERT_FALSE(actual.empty());

  // Every pixel must be transparent black (the lighting dispatch is skipped).
  EXPECT_THAT(PixelAt(actual, 32, 32), RgbaEq(0, 0, 0, 0));
  EXPECT_THAT(PixelAt(actual, 18, 18), IsTransparent())
      << "specularExponent<1 must produce transparent output everywhere";
}

// A lighting primitive without a light-source child has no light, so it produces transparent
// black, as feDiffuseLighting does on Geode and both primitives do on tiny-skia.
TEST_F(RendererGeodeTest, FilterSpecularLightingWithoutLightSourceIsTransparent) {
  components::FilterGraph graph;
  components::FilterNode specularNode;
  components::filter_primitive::SpecularLighting specular;
  specular.surfaceScale = 1.0;
  specular.specularConstant = 1.0;
  specular.specularExponent = 1.0;
  specular.lightingColor = css::Color(css::RGBA(255, 255, 255, 255));
  ASSERT_FALSE(specular.light.has_value());

  specularNode.primitive = specular;
  specularNode.inputs.push_back(components::FilterStandardInput::SourceGraphic);
  graph.nodes.push_back(specularNode);

  const RendererBitmap actual = renderFlatSourceThroughFilter(graph, Transform2d());
  ASSERT_FALSE(actual.empty());

  const size_t width = static_cast<size_t>(kViewportSize);
  const RendererBitmap expected{Vector2i(kViewportSize, kViewportSize),
                                std::vector<uint8_t>(width * width * 4), width * 4};
  editor::tests::CompareBitmapToBitmap(actual, expected, "specular_lighting_without_light_source",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST_F(RendererGeodeTest, FilterInvalidConvolveMatrixClearsReusedTexture) {
  auto renderGraph = [&](RendererGeode& renderer, const components::FilterGraph& filterGraph) {
    beginFrame(renderer);
    renderer.pushFilterLayer(filterGraph, Box2d({0, 0}, {kViewportSize, kViewportSize}));
    renderer.setPaint(solidFill(css::RGBA(255, 255, 255, 255)));
    renderer.setTransform(Transform2d());
    renderer.drawRect(Box2d({-4, -4}, {kViewportSize + 4, kViewportSize + 4}), StrokeParams{});
    renderer.popFilterLayer();
    renderer.endFrame();
    return renderer.takeSnapshot();
  };

  // Fill the entire filter-intermediate pool bucket with opaque red textures. Reacquiring one of
  // these textures must not leak its previous contents through a transparent short-circuit path.
  components::FilterGraph warmGraph;
  components::FilterNode floodNode;
  components::filter_primitive::Flood flood;
  flood.floodColor = css::Color(css::RGBA(255, 0, 0, 255));
  flood.floodOpacity = 1.0;
  floodNode.primitive = flood;
  for (int i = 0; i < 8; ++i) {
    warmGraph.nodes.push_back(floodNode);
  }

  RendererGeode warmRenderer = createRenderer();
  const RendererBitmap warm = renderGraph(warmRenderer, warmGraph);
  ASSERT_FALSE(warm.empty());
  EXPECT_THAT(PixelAt(warm, 32, 32), RgbaEq(255, 0, 0, 255));

  using ConvolveMatrix = components::filter_primitive::ConvolveMatrix;
  const auto makeGraph = [](const ConvolveMatrix& convolve) {
    components::FilterGraph graph;
    components::FilterNode convolveNode;
    convolveNode.primitive = convolve;
    convolveNode.inputs.push_back(components::FilterStandardInput::SourceGraphic);
    graph.nodes.push_back(convolveNode);
    return graph;
  };
  const auto expectTransparent = [](const RendererBitmap& actual) {
    ASSERT_THAT(actual.empty(), testing::IsFalse());
    EXPECT_THAT(PixelAt(actual, 32, 32), IsTransparent());
    EXPECT_THAT(PixelAt(actual, 18, 18), IsTransparent());
  };

  const std::array<std::pair<const char*, ConvolveMatrix>, 4> invalidCases{{
      {"zero order", ConvolveMatrix{.orderX = 0, .orderY = 1}},
      {"coefficient count", ConvolveMatrix{.orderX = 3, .orderY = 3, .kernelMatrix = {1.0}}},
      {"zero divisor",
       ConvolveMatrix{.orderX = 1, .orderY = 1, .kernelMatrix = {1.0}, .divisor = 0.0}},
      {"target outside kernel",
       ConvolveMatrix{.orderX = 1, .orderY = 1, .kernelMatrix = {1.0}, .targetX = 1}},
  }};

  for (const auto& [name, convolve] : invalidCases) {
    SCOPED_TRACE(name);
    RendererGeode invalidRenderer = createRenderer();
    expectTransparent(renderGraph(invalidRenderer, makeGraph(convolve)));
  }

  const ConvolveMatrix zeroDivisor{.orderX = 1, .orderY = 1, .kernelMatrix = {1.0}, .divisor = 0.0};
  const std::array<std::pair<const char*, ConvolveMatrix>, 3> nonfiniteCases{{
      {"non-finite divisor", ConvolveMatrix{.orderX = 1,
                                            .orderY = 1,
                                            .kernelMatrix = {1.0},
                                            .divisor = std::numeric_limits<double>::infinity()}},
      {"non-finite bias", ConvolveMatrix{.orderX = 1,
                                         .orderY = 1,
                                         .kernelMatrix = {1.0},
                                         .bias = std::numeric_limits<double>::quiet_NaN()}},
      {"non-finite coefficient",
       ConvolveMatrix{
           .orderX = 1, .orderY = 1, .kernelMatrix = {std::numeric_limits<double>::quiet_NaN()}}},
  }};

  for (const auto& [name, convolve] : nonfiniteCases) {
    SCOPED_TRACE(name);
    RendererGeode invalidRenderer = createRenderer();
    const components::FilterGraph controlGraph = makeGraph(zeroDivisor);
    const components::FilterGraph probeGraph = makeGraph(convolve);

    expectTransparent(renderGraph(invalidRenderer, controlGraph));
    expectTransparent(renderGraph(invalidRenderer, controlGraph));
    const geode::GeodeCounters controlBefore = invalidRenderer.lastFrameTimings().counters;
    expectTransparent(renderGraph(invalidRenderer, probeGraph));
    const geode::GeodeCounters probe = invalidRenderer.lastFrameTimings().counters;
    expectTransparent(renderGraph(invalidRenderer, controlGraph));
    const geode::GeodeCounters controlAfter = invalidRenderer.lastFrameTimings().counters;

    ASSERT_THAT(controlAfter.bufferWrites, testing::Eq(controlBefore.bufferWrites));
    ASSERT_THAT(controlAfter.bufferWriteBytes, testing::Eq(controlBefore.bufferWriteBytes));
    EXPECT_THAT(probe.bufferWrites, testing::Eq(controlBefore.bufferWrites))
        << "Invalid convolution parameters must be rejected before a parameter-buffer upload";
    EXPECT_THAT(probe.bufferWriteBytes, testing::Eq(controlBefore.bufferWriteBytes))
        << "Invalid convolution parameters must be rejected before parameter bytes are uploaded";
  }
}

TEST_F(RendererGeodeTest, FilterDiffuseLightingSpotLightConeMatchesCpuReference) {
  components::FilterGraph graph;
  // The CPU reference runs the raw lighting kernel in sRGB; pin the graph to sRGB
  // so geode matches (the linearRGB default is covered by the resvg suite).
  graph.colorInterpolationFilters = svg::ColorInterpolationFilters::SRGB;
  components::FilterNode diffuseNode;
  components::filter_primitive::DiffuseLighting diffuse;
  diffuse.surfaceScale = 2.0;
  diffuse.diffuseConstant = 1.0;
  diffuse.lightingColor = css::Color(css::RGBA(255, 255, 255, 255));

  components::filter_primitive::LightSource light;
  light.type = components::filter_primitive::LightSource::Type::Spot;
  light.x = 12.0;
  light.y = 20.0;
  light.z = 20.0;
  light.pointsAtX = 56.0;
  light.pointsAtY = 36.0;
  light.pointsAtZ = 0.0;
  light.spotExponent = 4.0;
  light.limitingConeAngle = 30.0;
  diffuse.light = light;

  diffuseNode.primitive = diffuse;
  diffuseNode.inputs.push_back(components::FilterStandardInput::SourceGraphic);
  graph.nodes.push_back(diffuseNode);

  const Transform2d deviceFromFilter = Transform2d();
  const RendererBitmap actual = renderFlatSourceThroughFilter(graph, deviceFromFilter);
  ASSERT_FALSE(actual.empty());
  const std::vector<uint8_t> expected =
      CpuDiffuseLightingReferenceForFlatSource(diffuse, deviceFromFilter);
  ASSERT_FALSE(expected.empty());

  const BitmapDiffStats diff = DiffBitmapAgainstStraightRgba(actual, expected, 1, 1);
  EXPECT_EQ(diff.differingChannels, 0)
      << "maxDiff=" << diff.maxChannelDiff << " first=(" << diff.firstDiffX << ", "
      << diff.firstDiffY << ") channel=" << diff.firstDiffChannel
      << " actual=" << static_cast<int>(diff.actualValue)
      << " expected=" << static_cast<int>(diff.expectedValue);
}

TEST_F(RendererGeodeTest, FilterEmptyMergeProducesTransparentBlack) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  components::FilterGraph graph;
  components::FilterNode mergeNode;
  mergeNode.primitive = components::filter_primitive::Merge{};
  graph.nodes.push_back(mergeNode);

  renderer.pushFilterLayer(graph, Box2d({0, 0}, {kViewportSize, kViewportSize}));

  renderer.setPaint(solidFill(css::RGBA(0, 255, 0, 255)));
  renderer.setTransform(Transform2d());
  renderer.drawRect(Box2d({16, 16}, {48, 48}), StrokeParams{});

  renderer.popFilterLayer();
  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  ASSERT_FALSE(snap.empty());

  EXPECT_THAT(PixelAt(snap, 32, 32), RgbaEq(0, 0, 0, 0));
}

/// feFlood: fill the filter region with a constant color.
TEST_F(RendererGeodeTest, FilterFloodFillsSubregion) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  components::FilterGraph graph;
  components::FilterNode floodNode;
  components::filter_primitive::Flood flood;
  flood.floodColor = css::Color(css::RGBA(255, 0, 0, 255));
  flood.floodOpacity = 0.5;
  floodNode.primitive = flood;
  // feFlood does not use an input.
  graph.nodes.push_back(floodNode);

  renderer.pushFilterLayer(graph, Box2d({0, 0}, {kViewportSize, kViewportSize}));

  // Draw something (feFlood ignores input, but the filter layer needs content
  // to establish the source-graphic texture dimensions).
  renderer.setPaint(solidFill(css::RGBA(0, 255, 0, 255)));
  renderer.setTransform(Transform2d());
  renderer.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});

  renderer.popFilterLayer();
  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  ASSERT_FALSE(snap.empty());

  // feFlood with red at 50% opacity. The GPU stores premultiplied (128,0,0,128)
  // but takeSnapshot() unpremultiplies to straight alpha: (255,0,0,128).
  auto center = PixelAt(snap, 32, 32);
  EXPECT_THAT(center, Rgba(Near(255, 2), testing::Eq(0), testing::Eq(0), Near(128, 2)))
      << "feFlood should preserve straight-alpha red at 50% opacity";
}

/// A filter pass recorded through the GPU runtime, on the far side of a command buffer split.
///
/// A large filter graph is split across several command buffers to bound how much one buffer
/// carries. Each split finishes and submits the encoder the runtime is replaying into and starts a
/// fresh one, and the runtime holds completion of everything it replayed until that submit is
/// reported to it. Until it is, and until the runtime is pointed at the replacement, a replayed
/// pass records into a command encoder that has already been consumed and its work never reaches
/// the queue.
///
/// Each morphology node at this radius decomposes into the per-axis pass cap, so the six of them
/// outrun the sixty-four passes one command buffer carries and the flood after them is recorded
/// past a split.
TEST_F(RendererGeodeTest, FloodAfterACommandBufferSplitStillReachesTheQueue) {
  constexpr size_t kMorphologyNodes = 6;

  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  components::FilterGraph graph;
  for (size_t i = 0; i < kMorphologyNodes; ++i) {
    components::FilterNode morphologyNode;
    components::filter_primitive::Morphology morphology;
    morphology.op = components::filter_primitive::Morphology::Operator::Dilate;
    morphology.radiusX = components::kMaximumFilterMorphologyRadius;
    morphology.radiusY = components::kMaximumFilterMorphologyRadius;
    morphologyNode.primitive = morphology;
    graph.nodes.push_back(morphologyNode);
  }

  components::FilterNode floodNode;
  components::filter_primitive::Flood flood;
  // feFlood ignores its input, so this node alone decides the result.
  flood.floodColor = css::Color(css::RGBA(0, 0, 255, 255));
  flood.floodOpacity = 1.0;
  floodNode.primitive = flood;
  graph.nodes.push_back(floodNode);

  renderer.pushFilterLayer(graph, Box2d({0, 0}, {kViewportSize, kViewportSize}));

  renderer.setPaint(solidFill(css::RGBA(0, 255, 0, 255)));
  renderer.setTransform(Transform2d());
  renderer.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});

  renderer.popFilterLayer();
  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  ASSERT_FALSE(snap.empty());

  EXPECT_THAT(PixelAt(snap, 32, 32), RgbaEq(0, 0, 255, 255))
      << "the flood recorded after the command buffer split must still reach the queue";
}

TEST_F(RendererGeodeTest, FilterImagePixelatedSmoothsFromNearestIntegerScale) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  components::filter_primitive::Image image;
  image.imageData = std::make_shared<const std::vector<uint8_t>>(std::initializer_list<uint8_t>{
      255, 0, 0, 255, 0, 0, 255, 255, 255, 0, 0, 255, 0, 0, 255, 255});
  image.imageWidth = 2;
  image.imageHeight = 2;
  image.imageRendering = ImageRendering::Pixelated;

  components::FilterGraph graph;
  components::FilterNode imageNode;
  imageNode.primitive = std::move(image);
  graph.nodes.push_back(std::move(imageNode));

  renderer.pushFilterLayer(graph, Box2d::FromXYWH(0.0, 0.0, 5.0, 5.0));
  renderer.setPaint(solidFill(css::RGBA(0, 0, 0, 0)));
  renderer.drawRect(Box2d::FromXYWH(0.0, 0.0, 5.0, 5.0), StrokeParams{});
  renderer.popFilterLayer();
  renderer.endFrame();

  const std::array<uint8_t, 4> center = PixelAt(renderer.takeSnapshot(), 2, 2);
  EXPECT_THAT(center, Rgba(::testing::Gt(0), 0, ::testing::Gt(0), 255));
}

TEST_F(RendererGeodeTest, FilterImageWithTrailingPayloadIsTransparent) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  components::filter_primitive::Image image;
  image.imageData = std::make_shared<const std::vector<uint8_t>>(
      std::initializer_list<uint8_t>{255, 0, 0, 255, 17});
  image.imageWidth = 1;
  image.imageHeight = 1;

  components::FilterGraph graph;
  components::FilterNode imageNode;
  imageNode.primitive = std::move(image);
  graph.nodes.push_back(std::move(imageNode));

  renderer.pushFilterLayer(graph, Box2d::FromXYWH(0.0, 0.0, 5.0, 5.0));
  renderer.setPaint(solidFill(css::RGBA(0, 0, 0, 0)));
  renderer.drawRect(Box2d::FromXYWH(0.0, 0.0, 5.0, 5.0), StrokeParams{});
  renderer.popFilterLayer();
  renderer.endFrame();

  EXPECT_THAT(PixelAt(renderer.takeSnapshot(), 2, 2), IsTransparent());
}

TEST_F(RendererGeodeTest, FilterImageOverTextureAxisLimitIsTransparent) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  ASSERT_LT(sharedDevice()->maxTextureDimension2D(),
            static_cast<uint32_t>(std::numeric_limits<int>::max()));
  const int overLimitWidth = static_cast<int>(sharedDevice()->maxTextureDimension2D()) + 1;
  components::filter_primitive::Image image;
  image.imageData = std::make_shared<const std::vector<uint8_t>>(
      static_cast<std::size_t>(overLimitWidth) * 4u, 255);
  image.imageWidth = overLimitWidth;
  image.imageHeight = 1;

  components::FilterGraph graph;
  components::FilterNode imageNode;
  imageNode.primitive = std::move(image);
  graph.nodes.push_back(std::move(imageNode));

  renderer.pushFilterLayer(graph, Box2d::FromXYWH(0.0, 0.0, 5.0, 5.0));
  renderer.setPaint(solidFill(css::RGBA(0, 0, 0, 0)));
  renderer.drawRect(Box2d::FromXYWH(0.0, 0.0, 5.0, 5.0), StrokeParams{});
  renderer.popFilterLayer();
  renderer.endFrame();

  const RendererBitmap snapshot = renderer.takeSnapshot();
  ASSERT_FALSE(snapshot.empty());
  EXPECT_THAT(PixelAt(snapshot, 2, 2), IsTransparent());
}

/// feMerge: composite two feFlood layers via alpha-over.
TEST_F(RendererGeodeTest, FilterMergeCompositesInputs) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  // Build a filter graph:
  //   Node 0: feFlood red 50% → result="red"
  //   Node 1: feFlood blue 50% → result="blue"
  //   Node 2: feMerge(in="red", in="blue") → alpha-over composite
  // This test validates the raw merge alpha-over math in sRGB, so pin the
  // graph to sRGB color-interpolation (the SVG default is linearRGB, exercised
  // by the resvg suite's feMerge/* parity tests instead).
  components::FilterGraph graph;
  graph.colorInterpolationFilters = svg::ColorInterpolationFilters::SRGB;

  // Node 0: red flood.
  {
    components::FilterNode node;
    components::filter_primitive::Flood f;
    f.floodColor = css::Color(css::RGBA(255, 0, 0, 255));
    f.floodOpacity = 0.5;
    node.primitive = f;
    node.result = RcString("red");
    graph.nodes.push_back(node);
  }

  // Node 1: blue flood.
  {
    components::FilterNode node;
    components::filter_primitive::Flood f;
    f.floodColor = css::Color(css::RGBA(0, 0, 255, 255));
    f.floodOpacity = 0.5;
    node.primitive = f;
    node.result = RcString("blue");
    graph.nodes.push_back(node);
  }

  // Node 2: feMerge with "red" and "blue" as inputs.
  {
    components::FilterNode node;
    node.primitive = components::filter_primitive::Merge{};
    node.inputs.push_back(components::FilterInput::Named{RcString("red")});
    node.inputs.push_back(components::FilterInput::Named{RcString("blue")});
    graph.nodes.push_back(node);
  }

  renderer.pushFilterLayer(graph, Box2d({0, 0}, {kViewportSize, kViewportSize}));

  renderer.setPaint(solidFill(css::RGBA(0, 255, 0, 255)));
  renderer.setTransform(Transform2d());
  renderer.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});

  renderer.popFilterLayer();
  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  ASSERT_FALSE(snap.empty());

  // GPU alpha-over of premul red(128,0,0,128) then premul blue(0,0,128,128):
  //   premul result ≈ (64, 0, 128, 192)
  // takeSnapshot() unpremultiplies: R=64*255/192≈85, B=128*255/192≈170, A=192.
  auto center = PixelAt(snap, 32, 32);
  EXPECT_THAT(center, Rgba(Near(85, 6), testing::Eq(0), Near(170, 6), testing::Gt(170)))
      << "feMerge should alpha-over red behind blue";
}

/// feComposite operator=in: result = in1 * in2.a.
/// With in1 = opaque red flood, in2 = 50% alpha green flood:
///   premul result = (255,0,0,255) * 0.5 = (128,0,0,128)
///   straight = (255,0,0,128)
TEST_F(RendererGeodeTest, FilterCompositeInOperator) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  components::FilterGraph graph;

  // Node 0: opaque red flood → result="red".
  {
    components::FilterNode node;
    components::filter_primitive::Flood f;
    f.floodColor = css::Color(css::RGBA(255, 0, 0, 255));
    f.floodOpacity = 1.0;
    node.primitive = f;
    node.result = RcString("red");
    graph.nodes.push_back(node);
  }

  // Node 1: 50% alpha green flood → result="green".
  {
    components::FilterNode node;
    components::filter_primitive::Flood f;
    f.floodColor = css::Color(css::RGBA(0, 255, 0, 255));
    f.floodOpacity = 0.5;
    node.primitive = f;
    node.result = RcString("green");
    graph.nodes.push_back(node);
  }

  // Node 2: feComposite in="red" in2="green" operator=in.
  {
    components::FilterNode node;
    components::filter_primitive::Composite comp;
    comp.op = components::filter_primitive::Composite::Operator::In;
    node.primitive = comp;
    node.inputs.push_back(components::FilterInput::Named{RcString("red")});
    node.inputs.push_back(components::FilterInput::Named{RcString("green")});
    graph.nodes.push_back(node);
  }

  renderer.pushFilterLayer(graph, Box2d({0, 0}, {kViewportSize, kViewportSize}));
  renderer.setPaint(solidFill(css::RGBA(0, 0, 255, 255)));
  renderer.setTransform(Transform2d());
  renderer.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});
  renderer.popFilterLayer();
  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  ASSERT_FALSE(snap.empty());

  // in1 premul = (255,0,0,255), in2 premul = (0,128,0,128).
  // operator=in: result = in1 * in2.a = (255,0,0,255) * (128/255) ≈ (128,0,0,128).
  // takeSnapshot unpremultiplies: R=128*255/128=255, A=128.
  auto center = PixelAt(snap, 32, 32);
  EXPECT_THAT(center, Rgba(Near(255, 3), testing::Eq(0), testing::Eq(0), Near(128, 3)))
      << "feComposite operator=in should mask red by green alpha";
}

/// feComposite with no `in2`: an unspecified `in2` resolves the same way an unspecified `in`
/// does, to the preceding primitive's result. Falling back to the node's own first input would
/// mask red by its own opaque alpha and leave the result fully opaque instead.
TEST_F(RendererGeodeTest, FilterCompositeImplicitIn2UsesPrecedingResult) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  components::FilterGraph graph;

  // Node 0: opaque red flood -> result="red".
  {
    components::FilterNode node;
    components::filter_primitive::Flood f;
    f.floodColor = css::Color(css::RGBA(255, 0, 0, 255));
    f.floodOpacity = 1.0;
    node.primitive = f;
    node.result = RcString("red");
    graph.nodes.push_back(node);
  }

  // Node 1: 50% alpha green flood, deliberately unnamed so it is reachable only as the previous
  // result.
  {
    components::FilterNode node;
    components::filter_primitive::Flood f;
    f.floodColor = css::Color(css::RGBA(0, 255, 0, 255));
    f.floodOpacity = 0.5;
    node.primitive = f;
    graph.nodes.push_back(node);
  }

  // Node 2: feComposite in="red" operator=in, with no in2 at all.
  {
    components::FilterNode node;
    components::filter_primitive::Composite comp;
    comp.op = components::filter_primitive::Composite::Operator::In;
    node.primitive = comp;
    node.inputs.push_back(components::FilterInput::Named{RcString("red")});
    graph.nodes.push_back(node);
  }

  renderer.pushFilterLayer(graph, Box2d({0, 0}, {kViewportSize, kViewportSize}));
  renderer.setPaint(solidFill(css::RGBA(0, 0, 255, 255)));
  renderer.setTransform(Transform2d());
  renderer.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});
  renderer.popFilterLayer();
  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  ASSERT_FALSE(snap.empty());

  // in1 premul = (255,0,0,255); the defaulted in2 is the green flood, premul alpha 128.
  // operator=in: result = in1 * in2.a ~= (128,0,0,128), which takeSnapshot unpremultiplies to
  // (255,0,0,128). Reading the first input as in2 would have left alpha at 255.
  auto center = PixelAt(snap, 32, 32);
  EXPECT_THAT(center, Rgba(Near(255, 3), testing::Eq(0), testing::Eq(0), Near(128, 3)))
      << "an unspecified in2 should mask red by the preceding flood's alpha";
}

/// feComposite defaults to operator=over.
TEST_F(RendererGeodeTest, FilterCompositeOverDefault) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  components::FilterGraph graph;

  // Node 0: opaque red flood → result="red".
  {
    components::FilterNode node;
    components::filter_primitive::Flood f;
    f.floodColor = css::Color(css::RGBA(255, 0, 0, 255));
    f.floodOpacity = 1.0;
    node.primitive = f;
    node.result = RcString("red");
    graph.nodes.push_back(node);
  }

  // Node 1: 50% alpha blue flood → result="blue".
  {
    components::FilterNode node;
    components::filter_primitive::Flood f;
    f.floodColor = css::Color(css::RGBA(0, 0, 255, 255));
    f.floodOpacity = 0.5;
    node.primitive = f;
    node.result = RcString("blue");
    graph.nodes.push_back(node);
  }

  // Node 2: feComposite in="blue" in2="red" (default operator=over).
  {
    components::FilterNode node;
    components::filter_primitive::Composite comp;
    // Default op is Over.
    node.primitive = comp;
    node.inputs.push_back(components::FilterInput::Named{RcString("blue")});
    node.inputs.push_back(components::FilterInput::Named{RcString("red")});
    graph.nodes.push_back(node);
  }

  renderer.pushFilterLayer(graph, Box2d({0, 0}, {kViewportSize, kViewportSize}));
  renderer.setPaint(solidFill(css::RGBA(0, 255, 0, 255)));
  renderer.setTransform(Transform2d());
  renderer.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});
  renderer.popFilterLayer();
  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  ASSERT_FALSE(snap.empty());

  // feComposite operates in linearRGB by default (the `color-interpolation-
  // filters` property), matching tiny-skia and the SVG spec. With pure blue
  // (B=1.0) and red (R=1.0) the sRGB→linear conversion is the identity on the
  // active channels, but the `over` blend mixes red's R into the 50%-alpha
  // backdrop region, so the result is computed in linear light:
  //   in1 = blue 50% linear-premul (0,0,0.502,0.502),
  //   in2 = red opaque linear-premul (1.0,0,0,1.0).
  //   over = in1 + in2*(1 - in1.a) = (0.498, 0, 0.502, 1.0) linear.
  //   linear→sRGB: 0.498 → ~0.735 → 187, 0.502 → ~0.738 → 188.
  // (Running this `over` in sRGB instead would give the wrong (127,0,128) - the
  // pre-linearRGB-fix behavior. See GeodeFilterEngine::execute feComposite wrap.)
  auto center = PixelAt(snap, 32, 32);
  // `over` in linearRGB → R≈187, G=0, B≈188, A≈255 (sRGB would give the wrong 127,0,128).
  EXPECT_THAT(center, Rgba(Near(187, 4), testing::Eq(0), Near(188, 4), Near(255, 1)))
      << "feComposite `over` must be evaluated in linearRGB";
}

/// feComposite operator=arithmetic with k1=1,k2=0,k3=0,k4=0 → in1*in2 (multiply).
TEST_F(RendererGeodeTest, FilterCompositeArithmetic) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  components::FilterGraph graph;

  // Node 0: opaque red flood → result="red".
  {
    components::FilterNode node;
    components::filter_primitive::Flood f;
    f.floodColor = css::Color(css::RGBA(255, 0, 0, 255));
    f.floodOpacity = 1.0;
    node.primitive = f;
    node.result = RcString("red");
    graph.nodes.push_back(node);
  }

  // Node 1: opaque white flood → result="white".
  {
    components::FilterNode node;
    components::filter_primitive::Flood f;
    f.floodColor = css::Color(css::RGBA(255, 255, 255, 255));
    f.floodOpacity = 1.0;
    node.primitive = f;
    node.result = RcString("white");
    graph.nodes.push_back(node);
  }

  // Node 2: feComposite operator=arithmetic k1=1 k2=0 k3=0 k4=0.
  // result = k1*in1*in2 = in1*in2. Red * White = Red.
  {
    components::FilterNode node;
    components::filter_primitive::Composite comp;
    comp.op = components::filter_primitive::Composite::Operator::Arithmetic;
    comp.k1 = 1.0;
    comp.k2 = 0.0;
    comp.k3 = 0.0;
    comp.k4 = 0.0;
    node.primitive = comp;
    node.inputs.push_back(components::FilterInput::Named{RcString("red")});
    node.inputs.push_back(components::FilterInput::Named{RcString("white")});
    graph.nodes.push_back(node);
  }

  renderer.pushFilterLayer(graph, Box2d({0, 0}, {kViewportSize, kViewportSize}));
  renderer.setPaint(solidFill(css::RGBA(0, 255, 0, 255)));
  renderer.setTransform(Transform2d());
  renderer.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});
  renderer.popFilterLayer();
  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  ASSERT_FALSE(snap.empty());

  // k1*red*white = red. Expected: (255,0,0,255).
  auto center = PixelAt(snap, 32, 32);
  EXPECT_THAT(center, Rgba(Near(255, 2), testing::Eq(0), testing::Eq(0), Near(255, 2)))
      << "Arithmetic composite should multiply red by white";
}

/// feBlend mode=multiply: two opaque flood inputs.
/// Red * Blue = (1,0,0) * (0,0,1) = (0,0,0). Result is black.
TEST_F(RendererGeodeTest, FilterBlendMultiply) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  components::FilterGraph graph;

  // Node 0: opaque red flood → result="red".
  {
    components::FilterNode node;
    components::filter_primitive::Flood f;
    f.floodColor = css::Color(css::RGBA(255, 0, 0, 255));
    f.floodOpacity = 1.0;
    node.primitive = f;
    node.result = RcString("red");
    graph.nodes.push_back(node);
  }

  // Node 1: opaque blue flood → result="blue".
  {
    components::FilterNode node;
    components::filter_primitive::Flood f;
    f.floodColor = css::Color(css::RGBA(0, 0, 255, 255));
    f.floodOpacity = 1.0;
    node.primitive = f;
    node.result = RcString("blue");
    graph.nodes.push_back(node);
  }

  // Node 2: feBlend in="red" in2="blue" mode=multiply.
  {
    components::FilterNode node;
    components::filter_primitive::Blend blend;
    blend.mode = components::filter_primitive::Blend::Mode::Multiply;
    node.primitive = blend;
    node.inputs.push_back(components::FilterInput::Named{RcString("red")});
    node.inputs.push_back(components::FilterInput::Named{RcString("blue")});
    graph.nodes.push_back(node);
  }

  renderer.pushFilterLayer(graph, Box2d({0, 0}, {kViewportSize, kViewportSize}));
  renderer.setPaint(solidFill(css::RGBA(0, 255, 0, 255)));
  renderer.setTransform(Transform2d());
  renderer.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});
  renderer.popFilterLayer();
  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  ASSERT_FALSE(snap.empty());

  // Multiply: red(1,0,0)*blue(0,0,1) = (0,0,0), both opaque → black opaque.
  auto center = PixelAt(snap, 32, 32);
  EXPECT_THAT(center, Rgba(testing::Eq(0), testing::Eq(0), testing::Eq(0), Near(255, 1)))
      << "Multiply blend should produce opaque black";
}

/// feBlend mode=screen: inverse of multiply.
/// Screen(R, B) = R+B-R*B = (1,0,0)+(0,0,1)-(0,0,0) = (1,0,1) = magenta.
TEST_F(RendererGeodeTest, FilterBlendScreen) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  components::FilterGraph graph;

  // Node 0: opaque red flood → result="red".
  {
    components::FilterNode node;
    components::filter_primitive::Flood f;
    f.floodColor = css::Color(css::RGBA(255, 0, 0, 255));
    f.floodOpacity = 1.0;
    node.primitive = f;
    node.result = RcString("red");
    graph.nodes.push_back(node);
  }

  // Node 1: opaque blue flood → result="blue".
  {
    components::FilterNode node;
    components::filter_primitive::Flood f;
    f.floodColor = css::Color(css::RGBA(0, 0, 255, 255));
    f.floodOpacity = 1.0;
    node.primitive = f;
    node.result = RcString("blue");
    graph.nodes.push_back(node);
  }

  // Node 2: feBlend in="red" in2="blue" mode=screen.
  {
    components::FilterNode node;
    components::filter_primitive::Blend blend;
    blend.mode = components::filter_primitive::Blend::Mode::Screen;
    node.primitive = blend;
    node.inputs.push_back(components::FilterInput::Named{RcString("red")});
    node.inputs.push_back(components::FilterInput::Named{RcString("blue")});
    graph.nodes.push_back(node);
  }

  renderer.pushFilterLayer(graph, Box2d({0, 0}, {kViewportSize, kViewportSize}));
  renderer.setPaint(solidFill(css::RGBA(0, 255, 0, 255)));
  renderer.setTransform(Transform2d());
  renderer.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});
  renderer.popFilterLayer();
  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  ASSERT_FALSE(snap.empty());

  // Screen: red+blue-red*blue = (1,0,1) = magenta, opaque.
  auto center = PixelAt(snap, 32, 32);
  EXPECT_THAT(center, Rgba(Near(255, 1), testing::Eq(0), Near(255, 1), Near(255, 1)))
      << "Screen blend should produce opaque magenta";
}

/// feMorphology dilate: expand a small white rect by 4 pixels.
TEST_F(RendererGeodeTest, FilterMorphologyDilateExpands) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  components::FilterGraph graph;
  components::FilterNode morphNode;
  components::filter_primitive::Morphology morph;
  morph.op = components::filter_primitive::Morphology::Operator::Dilate;
  morph.radiusX = 4.0;
  morph.radiusY = 4.0;
  morphNode.primitive = morph;
  morphNode.inputs.push_back(components::FilterStandardInput::SourceGraphic);
  graph.nodes.push_back(morphNode);

  renderer.pushFilterLayer(graph, Box2d({0, 0}, {kViewportSize, kViewportSize}));

  // Draw a small white rect in the center: (22,22)-(42,42) = 20×20.
  renderer.setPaint(solidFill(css::RGBA(255, 255, 255, 255)));
  renderer.setTransform(Transform2d());
  renderer.drawRect(Box2d({22, 22}, {42, 42}), StrokeParams{});

  renderer.popFilterLayer();
  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  ASSERT_FALSE(snap.empty());

  // After dilate r=4, the rect expands by 4 in each direction.
  // Pixel at (22-3, 32) = (19, 32) should be white (inside expanded region).
  auto expanded = PixelAt(snap, 19, 32);
  EXPECT_THAT(expanded, Rgba(testing::Gt(200), testing::_, testing::_, testing::Gt(200)))
      << "Dilate should expand the shape outward";

  // Center should still be white.
  auto center = PixelAt(snap, 32, 32);
  EXPECT_THAT(center, RgbaEq(255, 255, 255, 255)) << "Center should remain white";
}

/// feMorphology erode: shrink a rect by 4 pixels.
TEST_F(RendererGeodeTest, FilterMorphologyErodeShrinks) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  components::FilterGraph graph;
  components::FilterNode morphNode;
  components::filter_primitive::Morphology morph;
  morph.op = components::filter_primitive::Morphology::Operator::Erode;
  morph.radiusX = 4.0;
  morph.radiusY = 4.0;
  morphNode.primitive = morph;
  morphNode.inputs.push_back(components::FilterStandardInput::SourceGraphic);
  graph.nodes.push_back(morphNode);

  renderer.pushFilterLayer(graph, Box2d({0, 0}, {kViewportSize, kViewportSize}));

  // Draw a white rect (16,16)-(48,48) = 32×32.
  renderer.setPaint(solidFill(css::RGBA(255, 255, 255, 255)));
  renderer.setTransform(Transform2d());
  renderer.drawRect(Box2d({16, 16}, {48, 48}), StrokeParams{});

  renderer.popFilterLayer();
  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  ASSERT_FALSE(snap.empty());

  // After erode r=4, the rect shrinks by 4 inward from each edge.
  // Corner pixel (17, 17) should now be transparent (eroded away).
  auto corner = PixelAt(snap, 17, 17);
  EXPECT_THAT(corner, IsTransparent()) << "Corner should be transparent after erode";

  // Center (32, 32) should still be white (far from edges).
  auto center = PixelAt(snap, 32, 32);
  EXPECT_THAT(center, RgbaEq(255, 255, 255, 255)) << "Center should remain white";
}

/// feComponentTransfer identity: all channels pass through unchanged.
TEST_F(RendererGeodeTest, FilterComponentTransferIdentityPasses) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  components::FilterGraph graph;
  components::FilterNode ctNode;
  components::filter_primitive::ComponentTransfer ct;
  // All 4 channels default to Identity - no explicit setup needed.
  ctNode.primitive = ct;
  ctNode.inputs.push_back(components::FilterStandardInput::SourceGraphic);
  graph.nodes.push_back(ctNode);

  renderer.pushFilterLayer(graph, Box2d({0, 0}, {kViewportSize, kViewportSize}));

  renderer.setPaint(solidFill(css::RGBA(128, 64, 200, 255)));
  renderer.setTransform(Transform2d());
  renderer.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});

  renderer.popFilterLayer();
  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  ASSERT_FALSE(snap.empty());

  auto center = PixelAt(snap, 32, 32);
  EXPECT_THAT(center, Rgba(Near(128, 2), Near(64, 2), Near(200, 2), Near(255, 1)))
      << "Identity component transfer should pass all channels through";
}

/// feComponentTransfer gamma: apply x² to the red channel.
TEST_F(RendererGeodeTest, FilterComponentTransferGammaInverts) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  components::FilterGraph graph;
  components::FilterNode ctNode;
  components::filter_primitive::ComponentTransfer ct;
  // R: gamma with amplitude=1, exponent=2, offset=0 → R' = R².
  ct.funcR.type = components::filter_primitive::ComponentTransfer::FuncType::Gamma;
  ct.funcR.amplitude = 1.0;
  ct.funcR.exponent = 2.0;
  ct.funcR.offset = 0.0;
  ctNode.primitive = ct;
  ctNode.inputs.push_back(components::FilterStandardInput::SourceGraphic);
  graph.nodes.push_back(ctNode);

  renderer.pushFilterLayer(graph, Box2d({0, 0}, {kViewportSize, kViewportSize}));

  // Draw a rect with R=128/255 ≈ 0.502. After R² ≈ 0.252 → ~64.
  renderer.setPaint(solidFill(css::RGBA(128, 0, 0, 255)));
  renderer.setTransform(Transform2d());
  renderer.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});

  renderer.popFilterLayer();
  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  ASSERT_FALSE(snap.empty());

  auto center = PixelAt(snap, 32, 32);
  // R=128/255≈0.502, R²≈0.252, premultiplied: R*A=0.252*1.0=0.252 → ~64.
  EXPECT_THAT(center, Rgba(Near(64, 4), testing::Eq(0), testing::Eq(0), Near(255, 1)))
      << "Gamma component transfer should square the red channel";
}

TEST_F(RendererGeodeTest, FilterTurbulenceBoundsNonFiniteSeedAndFrequency) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  components::FilterGraph graph;
  components::FilterNode node;
  components::filter_primitive::Turbulence turbulence;
  turbulence.baseFrequencyX = std::numeric_limits<double>::infinity();
  turbulence.baseFrequencyY = std::numeric_limits<double>::quiet_NaN();
  turbulence.numOctaves = 1;
  turbulence.seed = -std::numeric_limits<double>::infinity();
  node.primitive = turbulence;
  graph.nodes.push_back(node);

  renderer.pushFilterLayer(graph, Box2d({0, 0}, {kViewportSize, kViewportSize}));
  renderer.popFilterLayer();
  renderer.endFrame();

  EXPECT_FALSE(renderer.takeSnapshot().empty());
}

/// feConvolveMatrix box blur: 3×3 kernel of all 1s / divisor=9.
TEST_F(RendererGeodeTest, FilterConvolveMatrixBoxBlur) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  components::FilterGraph graph;
  components::FilterNode convNode;
  components::filter_primitive::ConvolveMatrix conv;
  conv.orderX = 3;
  conv.orderY = 3;
  conv.kernelMatrix = {1, 1, 1, 1, 1, 1, 1, 1, 1};
  conv.divisor = 9.0;
  conv.bias = 0.0;
  conv.edgeMode = components::filter_primitive::ConvolveMatrix::EdgeMode::Duplicate;
  conv.preserveAlpha = false;
  convNode.primitive = conv;
  convNode.inputs.push_back(components::FilterStandardInput::SourceGraphic);
  graph.nodes.push_back(convNode);

  renderer.pushFilterLayer(graph, Box2d({0, 0}, {kViewportSize, kViewportSize}));

  // Draw a crisp white rect (16,16)-(48,48).
  renderer.setPaint(solidFill(css::RGBA(255, 255, 255, 255)));
  renderer.setTransform(Transform2d());
  renderer.drawRect(Box2d({16, 16}, {48, 48}), StrokeParams{});

  renderer.popFilterLayer();
  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  ASSERT_FALSE(snap.empty());

  // Center should still be near-white (all neighbours are white).
  auto center = PixelAt(snap, 32, 32);
  EXPECT_THAT(center, Rgba(testing::Gt(240), testing::_, testing::_, testing::_))
      << "Center R should be near-white after box blur";

  // Edge pixel (16, 32): 3×3 kernel straddles the edge, so the value
  // should be intermediate (not fully white, not fully transparent).
  auto edge = PixelAt(snap, 16, 32);
  EXPECT_THAT(edge, Alpha(testing::AllOf(testing::Gt(50), testing::Lt(250))))
      << "Edge pixel should have intermediate alpha after box blur";
}

/// feConvolveMatrix edge detection: Laplacian kernel on a rect.
TEST_F(RendererGeodeTest, FilterConvolveMatrixEdgeDetect) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  components::FilterGraph graph;
  components::FilterNode convNode;
  components::filter_primitive::ConvolveMatrix conv;
  conv.orderX = 3;
  conv.orderY = 3;
  // Laplacian: [[0,-1,0],[-1,4,-1],[0,-1,0]].
  conv.kernelMatrix = {0, -1, 0, -1, 4, -1, 0, -1, 0};
  conv.divisor = 1.0;
  conv.bias = 0.0;
  conv.edgeMode = components::filter_primitive::ConvolveMatrix::EdgeMode::Duplicate;
  conv.preserveAlpha = false;
  convNode.primitive = conv;
  convNode.inputs.push_back(components::FilterStandardInput::SourceGraphic);
  graph.nodes.push_back(convNode);

  renderer.pushFilterLayer(graph, Box2d({0, 0}, {kViewportSize, kViewportSize}));

  // Draw a white rect (16,16)-(48,48).
  renderer.setPaint(solidFill(css::RGBA(255, 255, 255, 255)));
  renderer.setTransform(Transform2d());
  renderer.drawRect(Box2d({16, 16}, {48, 48}), StrokeParams{});

  renderer.popFilterLayer();
  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  ASSERT_FALSE(snap.empty());

  // Interior pixel (32, 32): all neighbours are identical white, so
  // Laplacian = 4*1 - 4*1 = 0. Output should be near-black/transparent.
  auto interior = PixelAt(snap, 32, 32);
  EXPECT_THAT(interior, Rgba(testing::Lt(20), testing::_, testing::_, testing::_))
      << "Interior should be near-black (Laplacian=0)";
}

/// SVG 2 §15.5: when an element has BOTH a `clip-path` and a `filter`, the
/// SourceGraphic for the filter must be the UNCLIPPED element content. The
/// clip is then applied to the FILTERED result, not to the input.
///
/// Repro: outer left-half clip + Gaussian blur of stdDev≈3 over a solid blue
/// rect that fully covers the viewport. Correct §15.5 behaviour produces:
///   - Pixels well inside the left half are uniform opaque blue (the blur of
///     a constant region is the same constant).
///   - Pixels well to the right of the clip boundary are transparent (the
///     filtered result is clipped after blurring).
///
/// The buggy path applies the clip BEFORE the filter, so the blur of the
/// already-clipped half-rect leaks energy inward and the interior pixels
/// near the clip edge end up dimmer/translucent.
TEST_F(RendererGeodeTest, FilterAppliedBeforeClipPathSvgRenderingOrder) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  // Outer clip: left half of the viewport, defined as a path so it goes
  // through the path-clip-mask code path (the same one resvg with-clip-path
  // exercises).
  ResolvedClip clip;
  ClipPathShape clipShape;
  clipShape.fillRule = FillRule::NonZero;
  clipShape.path = PathBuilder()
                       .moveTo(Vector2d(0.0, 0.0))
                       .lineTo(Vector2d(32.0, 0.0))
                       .lineTo(Vector2d(32.0, kViewportSize))
                       .lineTo(Vector2d(0.0, kViewportSize))
                       .closePath()
                       .build();
  clip.clipPaths.push_back(std::move(clipShape));
  renderer.pushClip(clip);

  components::FilterGraph graph;
  {
    components::FilterNode node;
    components::filter_primitive::GaussianBlur blur;
    blur.stdDeviationX = 3.0;
    blur.stdDeviationY = 3.0;
    node.primitive = blur;
    graph.nodes.push_back(node);
  }
  renderer.pushFilterLayer(graph, Box2d({0, 0}, {kViewportSize, kViewportSize}));

  renderer.setPaint(solidFill(css::RGBA(0, 0, 255, 255)));
  renderer.setTransform(Transform2d());
  renderer.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});

  renderer.popFilterLayer();
  renderer.popClip();
  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  ASSERT_FALSE(snap.empty());

  // (30, 32): inside the clipped region (clip is x<32) but only 2 pixels
  // from the right edge. The blur kernel (stdDev=3) at this x-coordinate
  // straddles the boundary heavily. Per §15.5, the SourceGraphic is the
  // UNCLIPPED full-viewport blue rect so the blur is uniform blue and this
  // pixel - being inside the clip - must remain near-opaque blue. The buggy
  // path captures only the left-half blue and the blur averages with
  // transparent-black, so the alpha drops to ~50% (~177) here.
  auto nearEdgeInside = PixelAt(snap, 30, 32);
  EXPECT_THAT(nearEdgeInside, Alpha(testing::Gt(240)))
      << "Near-edge inside-clip alpha should remain near-opaque (≥240). The "
         "buggy path blurs the already-clipped half-rect and pulls alpha "
         "toward ~50% here.";

  // (34, 32): just OUTSIDE the clip rect. Per §15.5 the clip is applied to
  // the FILTERED result, so this pixel must be fully transparent regardless
  // of the blur radius. The buggy path leaks blur energy outside the clip
  // boundary because the composite isn't gated by the clip mask, leaving
  // alpha around 50.
  auto justOutside = PixelAt(snap, 34, 32);
  EXPECT_THAT(justOutside, IsTransparent())
      << "Just-outside-clip alpha must be 0 - clip-path is applied to the "
         "filtered result.";

  // (8, 32): well inside the clipped region, far from the boundary. Both
  // correct and buggy paths produce near-opaque blue here, but check anyway
  // as a sanity gate.
  auto deepInside = PixelAt(snap, 8, 32);
  EXPECT_THAT(deepInside, Rgba(testing::_, testing::_, testing::Gt(230), testing::Gt(230)))
      << "Deep-inside pixel should be near-opaque blue";

  // (48, 32): well outside the clip rect, far from the boundary. The clip is
  // applied AFTER the filter, so this pixel must be fully transparent.
  auto outside = PixelAt(snap, 48, 32);
  EXPECT_THAT(outside, IsTransparent()) << "Outside the clip must be transparent - the "
                                           "filter result is clipped on composite";
}

// A GeodeDevice that fails to initialize (e.g. no Vulkan adapter on a GPU-less
// worker) leaves RendererGeode in \"no-op mode\". Every rendering entry point
// must be a clean no-op rather than dereferencing the null device. The
// historical failure was a SIGSEGV in
// drawPath -> Impl::getFillEncode -> GeodeDevice::countPathEncode() when the
// resvg suite ran on a worker whose adapter acquisition failed.
TEST(RendererGeodeNullDeviceTest, ReadbackStatsAreEmpty) {
  RendererGeode renderer(std::shared_ptr<geode::GeodeDevice>(nullptr), /*verbose=*/false);

  const RendererReadbackStats stats = renderer.consumeReadbackStats();

  EXPECT_EQ(stats.count, 0);
  EXPECT_EQ(stats.pollIterations, 0);
  EXPECT_FALSE(stats.usedTimedWaitAny);
}

TEST_F(RendererGeodeTest, NullDeviceEntersNoOpModeWithoutCrashing) {
  // Deterministically simulate CreateHeadless() having returned nullptr by
  // handing the renderer a null device. Does not depend on the host GPU.
  RendererGeode renderer(std::shared_ptr<geode::GeodeDevice>(nullptr), /*verbose=*/false);

  // Low-level ops (direct callers such as the editor overlay) must no-op.
  RenderViewport viewport;
  viewport.size = Vector2d(kViewportSize, kViewportSize);
  viewport.devicePixelRatio = 1.0;
  renderer.beginFrame(viewport);
  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  renderer.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});
  renderer.endFrame();
  EXPECT_TRUE(renderer.takeSnapshot().empty())
      << "No-op mode has no rendered target, so the snapshot is empty.";

  // Full document render path via draw(). The draw() guard makes this a
  // clean no-op before the driver runs; the drawPath guard (the historical
  // crash site drawPath -> getFillEncode -> countPathEncode) is exercised
  // directly by the drawRect() call above, which routes through drawPath.
  ParseWarningSink warningSink;
  auto maybeDocument = parser::SVGParser::ParseSVG(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">
             <rect x="1" y="1" width="14" height="14" fill="red"/>
           </svg>)svg",
      warningSink);
  ASSERT_FALSE(maybeDocument.hasError()) << maybeDocument.error();
  SVGDocument document = std::move(maybeDocument).result();
  RendererUtils::prepareDocumentForRendering(document, /*verbose=*/false, warningSink);
  renderer.draw(document);  // Must not crash.
  EXPECT_TRUE(renderer.takeSnapshot().empty());
}

/// A fill whose subpaths are all vertical segments produces horizontal band
/// data but an EMPTY vertical band array, so the vertical band binding falls
/// back to the zero-filled slot every empty storage class reserves. WebGPU
/// requires a storage binding to expose at least one element of the WGSL
/// array it feeds, and `array<Band>` has an 8-byte stride, so a narrower
/// dummy fails bind-group validation at submit, invalidates the whole
/// command buffer of that frame, and takes the process down with it. The
/// rectangle drawn alongside the degenerate path is the liveness signal: its
/// pixels only survive if the frame actually submitted.
///
/// Unclipped, which takes the resident per-slot bindings.
TEST_F(RendererGeodeTest, VerticalOnlyFillWithEmptyVerticalBandsRenders) {
  ParseWarningSink warningSink;
  auto maybeDocument = parser::SVGParser::ParseSVG(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
             <rect x="0" y="0" width="64" height="64" fill="#00ff00"/>
             <path d="M 16 8 L 16 56 M 48 8 L 48 56" fill="#0000ff"/>
           </svg>)svg",
      warningSink);
  ASSERT_FALSE(maybeDocument.hasError()) << maybeDocument.error();
  SVGDocument document = std::move(maybeDocument).result();

  RendererGeode renderer = createRenderer();
  renderer.draw(document);
  const RendererBitmap snapshot = renderer.takeSnapshot();
  ASSERT_FALSE(snapshot.empty());
  EXPECT_THAT(PixelAt(snapshot, 32, 32), RgbaEq(0, 255, 0, 255))
      << "A fill with no vertical bands must not invalidate the frame it is drawn in.";
}

/// Clipped, which takes the per-frame arena bindings instead of the resident
/// ones. Same defect, different allocation path.
TEST_F(RendererGeodeTest, ClippedVerticalOnlyFillWithEmptyVerticalBandsRenders) {
  ParseWarningSink warningSink;
  auto maybeDocument = parser::SVGParser::ParseSVG(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
             <clipPath id="c"><rect x="0" y="0" width="64" height="64"/></clipPath>
             <g clip-path="url(#c)">
               <rect x="0" y="0" width="64" height="64" fill="#00ff00"/>
               <path d="M 16 8 L 16 56 M 48 8 L 48 56" fill="#0000ff"/>
             </g>
           </svg>)svg",
      warningSink);
  ASSERT_FALSE(maybeDocument.hasError()) << maybeDocument.error();
  SVGDocument document = std::move(maybeDocument).result();

  RendererGeode renderer = createRenderer();
  renderer.draw(document);
  const RendererBitmap snapshot = renderer.takeSnapshot();
  ASSERT_FALSE(snapshot.empty());
  EXPECT_THAT(PixelAt(snapshot, 32, 32), RgbaEq(0, 255, 0, 255))
      << "A clipped fill with no vertical bands must not invalidate the frame it is drawn in.";
}

#if defined(__APPLE__)
TEST_F(RendererGeodeTest, BatchedArenaUploadsStayWithinNormalMetalLimit) {
  std::string source =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
             <defs><clipPath id="clip"><rect width="64" height="64"/></clipPath></defs>
             <g clip-path="url(#clip)">)svg";
  source.reserve(300000);
  for (int i = 0; i < 4000; ++i) {
    source += "<rect x=\"" + std::to_string(i % 64) + "\" y=\"" + std::to_string((i / 64) % 64) +
              "\" width=\"1\" height=\"1\" fill=\"red\"/>";
  }
  source += "</g></svg>";

  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(source, warnings);
  ASSERT_FALSE(parsed.hasError()) << parsed.error();
  SVGDocument busyDocument = std::move(parsed).result();
  auto simpleParsed = parser::SVGParser::ParseSVG(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
             <rect width="64" height="64" fill="lime"/>
             </svg>)svg",
      warnings);
  ASSERT_FALSE(simpleParsed.hasError()) << simpleParsed.error();
  SVGDocument simpleDocument = std::move(simpleParsed).result();

  std::shared_ptr<geode::GeodeDevice> device(geode::GeodeDevice::CreateHeadless());
  ASSERT_NE(device, nullptr);
  RendererGeode renderer(device);
  auto& metal = static_cast<gpu::metal::MetalDevice&>(device->runtimeDevice());
  ASSERT_THAT(metal.pauseSubmissionsForTest(), gpu::IsOk());
  renderer.draw(busyDocument);
  renderer.draw(busyDocument);
  EXPECT_EQ(metal.writeStatsForTest().pendingWrites, 0u);
  EXPECT_THAT(renderer.deviceLost(), testing::Eq(false));
  // Combining small writes keeps this workload below the normal pending-write limit.
  EXPECT_THAT(renderer.takeTextureSnapshot(), testing::NotNull());
  metal.resumeSubmissionsForTest();
  ASSERT_EQ(device->waitForQueueIdle(), geode::GpuWaitResult::Complete);

  renderer.draw(busyDocument);
  const RendererBitmap retried = renderer.takeSnapshot();
  ASSERT_FALSE(retried.empty());
  EXPECT_THAT(PixelAt(retried, 32, 32), RgbaEq(255, 0, 0, 255));

  renderer.draw(simpleDocument);
  const RendererBitmap snapshot = renderer.takeSnapshot();
  ASSERT_FALSE(snapshot.empty());
  EXPECT_THAT(PixelAt(snapshot, 32, 32), RgbaEq(0, 255, 0, 255));
}

TEST_F(RendererGeodeTest, RecoverableArenaTexturesWaitForSharedDeviceSubmission) {
  std::shared_ptr<geode::GeodeDevice> device(geode::GeodeDevice::CreateHeadless());
  ASSERT_THAT(device, testing::NotNull());
  auto& metal = static_cast<gpu::metal::MetalDevice&>(device->runtimeDevice());
  ASSERT_THAT(metal.pauseSubmissionsForTest(), gpu::IsOk());

  RendererGeode producer(device);
  beginFrame(producer);
  producer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  producer.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});
  producer.endFrame();
  const uint64_t pendingSerial = device->runtimeDevice().lastSubmittedSerial();
  ASSERT_THAT(pendingSerial, testing::Gt(device->runtimeDevice().completedSerial()));

  RendererGeode refused(device);
  beginFrame(refused);
  components::FilterGraph graph;
  components::FilterNode blur;
  blur.primitive =
      components::filter_primitive::GaussianBlur{.stdDeviationX = 4, .stdDeviationY = 2};
  graph.nodes.push_back(blur);
  refused.setTransform(Transform2d::SkewX(0.2));
  refused.pushFilterLayer(graph, Box2d({0, 0}, {kViewportSize, kViewportSize}));
  refused.setTransform(Transform2d());
  refused.setPaint(solidFill(css::RGBA(255, 255, 255, 255)));
  refused.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});
  refused.injectTransformedResampleArenaWriteFailureForTesting();
  refused.popFilterLayer();
  refused.endFrame();
  EXPECT_THAT(refused.deviceLost(), testing::IsFalse());
  EXPECT_THAT(device->pendingFencedTextureBackings(), testing::Gt(0u));

  RendererGeode sibling(device);
  beginFrame(sibling);
  EXPECT_THAT(sibling.hasActiveDrawingEncoderForTesting(), testing::IsFalse());
  EXPECT_THAT(sibling.takeSnapshot().empty(), testing::IsTrue());
  sibling.endFrame();

  metal.resumeSubmissionsForTest();
  ASSERT_THAT(device->waitForQueueIdle(), testing::Eq(geode::GpuWaitResult::Complete));
  beginFrame(sibling);
  EXPECT_THAT(device->pendingFencedTextureBackings(), testing::Eq(0u));
  sibling.setPaint(solidFill(css::RGBA(0, 255, 0, 255)));
  sibling.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});
  sibling.endFrame();
  const RendererBitmap recovered = sibling.takeSnapshot();
  ASSERT_THAT(recovered.empty(), testing::IsFalse());
  EXPECT_THAT(PixelAt(recovered, 32, 32), RgbaEq(0, 255, 0, 255));
}
#endif

TEST_F(RendererGeodeTest, ArenaWriteRefusalBlocksManualFrameSnapshotsBeforeEnd) {
  std::shared_ptr<geode::GeodeDevice> device(geode::GeodeDevice::CreateHeadless());
  ASSERT_THAT(device, testing::NotNull());
  RendererGeode renderer(device);
  beginFrame(renderer);
  renderer.injectActiveArenaWriteFailureForTesting();
  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  renderer.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});
  EXPECT_THAT(renderer.deviceLost(), testing::IsFalse());
  EXPECT_THAT(renderer.borrowTextureSnapshot(), testing::IsNull());
  EXPECT_THAT(renderer.takeTextureSnapshot(), testing::IsNull());
  EXPECT_THAT(renderer.takeSnapshot().empty(), testing::IsTrue());
  ASSERT_THAT(device->counters(), testing::NotNull());
  const uint64_t submitsBefore = device->counters()->submits;
  renderer.endFrame();
  EXPECT_THAT(device->counters()->submits, testing::Eq(submitsBefore));

  beginFrame(renderer);
  renderer.setPaint(solidFill(css::RGBA(0, 255, 0, 255)));
  renderer.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});
  renderer.endFrame();
  const RendererBitmap recovered = renderer.takeSnapshot();
  ASSERT_THAT(recovered.empty(), testing::IsFalse());
  EXPECT_THAT(PixelAt(recovered, 32, 32), RgbaEq(0, 255, 0, 255));
}

TEST_F(RendererGeodeTest, RuntimeSnapshotRejectsWrongFormatWithoutConsumingTheTexture) {
  auto created = sharedDevice()->runtimeDevice().createTexture(
      {"snapshot",
       {4, 4},
       gpu::TextureFormat::RGBA8Unorm,
       gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc});
  ASSERT_FALSE(created.hasError()) << created.error();
  gpu::Texture texture = std::move(created).result();
  {
    auto snapshot = RendererGeodeTextureSnapshot::AdoptRuntimeTexture(
        sharedDevice(), std::move(texture), {4, 4}, gpu::TextureFormat::BGRA8Unorm,
        AlphaType::Premultiplied);
    EXPECT_THAT(snapshot.isValid(), testing::IsFalse());
    EXPECT_THAT(texture.isValid(), testing::IsTrue());
  }
  if (texture.isValid()) {
    (void)sharedDevice()->runtimeDevice().destroyTextureBacking(std::move(texture));
  }
}

TEST_F(RendererGeodeTest, RuntimeSnapshotRejectsInvalidContentBeforeTakingOwnership) {
  for (const Vector2i dimensions :
       {Vector2i(0, 4), Vector2i(-1, 4), Vector2i(5, 4), Vector2i(4, 5)}) {
    SCOPED_TRACE(dimensions);
    auto created = sharedDevice()->runtimeDevice().createTexture(
        {"snapshot",
         {4, 4},
         gpu::TextureFormat::RGBA8Unorm,
         gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc});
    ASSERT_FALSE(created.hasError()) << created.error();
    gpu::Texture texture = std::move(created).result();
    {
      auto snapshot = RendererGeodeTextureSnapshot::AdoptRuntimeTexture(
          sharedDevice(), std::move(texture), dimensions, gpu::TextureFormat::RGBA8Unorm,
          AlphaType::Premultiplied);
      EXPECT_THAT(snapshot.isValid(), testing::IsFalse());
      EXPECT_THAT(texture.isValid(), testing::IsTrue());
    }
    if (texture.isValid()) {
      (void)sharedDevice()->runtimeDevice().destroyTextureBacking(std::move(texture));
    }
  }
}

TEST_F(RendererGeodeTest, RuntimeSnapshotRejectsForeignOwnerWithoutConsumingTheTexture) {
  auto other = std::shared_ptr<geode::GeodeDevice>(geode::GeodeDevice::CreateHeadless());
  ASSERT_THAT(other, testing::NotNull());
  auto created = sharedDevice()->runtimeDevice().createTexture(
      {"snapshot",
       {4, 4},
       gpu::TextureFormat::RGBA8Unorm,
       gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc});
  ASSERT_FALSE(created.hasError()) << created.error();
  gpu::Texture texture = std::move(created).result();
  {
    auto snapshot = RendererGeodeTextureSnapshot::AdoptRuntimeTexture(
        other, std::move(texture), {4, 4}, gpu::TextureFormat::RGBA8Unorm,
        AlphaType::Premultiplied);
    EXPECT_THAT(snapshot.isValid(), testing::IsFalse());
    EXPECT_THAT(texture.isValid(), testing::IsTrue());
  }
  if (texture.isValid()) {
    (void)sharedDevice()->runtimeDevice().destroyTextureBacking(std::move(texture));
  }
}

TEST_F(RendererGeodeTest, SnapshotContentCannotGrowBeyondItsBacking) {
  auto created = sharedDevice()->runtimeDevice().createTexture(
      {"snapshot",
       {4, 4},
       gpu::TextureFormat::RGBA8Unorm,
       gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc});
  ASSERT_FALSE(created.hasError()) << created.error();
  gpu::Texture texture = std::move(created).result();
  auto snapshot = RendererGeodeTextureSnapshot::AdoptRuntimeTexture(
      sharedDevice(), std::move(texture), {2, 3}, gpu::TextureFormat::RGBA8Unorm,
      AlphaType::Premultiplied);
  snapshot.setDimensions({5, 3});
  EXPECT_THAT(snapshot.dimensions(), testing::Eq(Vector2i(2, 3)));
  snapshot.setDimensions({2, -1});
  EXPECT_THAT(snapshot.dimensions(), testing::Eq(Vector2i(2, 3)));
}

TEST_F(RendererGeodeTest, RuntimeSnapshotRejectsNullOwnerAndUnsupportedFormatWithoutConsumption) {
  for (const bool missingOwner : {false, true}) {
    SCOPED_TRACE(missingOwner);
    const auto format = missingOwner ? gpu::TextureFormat::RGBA8Unorm : gpu::TextureFormat::R8Unorm;
    auto created = sharedDevice()->runtimeDevice().createTexture(
        {"snapshot", {4, 4}, format, gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc});
    ASSERT_FALSE(created.hasError()) << created.error();
    gpu::Texture texture = std::move(created).result();
    {
      auto snapshot = RendererGeodeTextureSnapshot::AdoptRuntimeTexture(
          missingOwner ? nullptr : sharedDevice(), std::move(texture), {4, 4},
          missingOwner ? gpu::TextureFormat::RGBA8Unorm : gpu::TextureFormat::R8Unorm,
          AlphaType::Premultiplied);
      EXPECT_THAT(snapshot.isValid(), testing::IsFalse());
      EXPECT_THAT(texture.isValid(), testing::IsTrue());
    }
    if (texture.isValid()) {
      (void)sharedDevice()->runtimeDevice().destroyTextureBacking(std::move(texture));
    }
  }
}

TEST_F(RendererGeodeTest, RuntimeSnapshotRejectsStaleIdentityWithoutTouchingItsReplacement) {
  gpu::Device& runtime = sharedDevice()->runtimeDevice();
  const gpu::TextureDescriptor descriptor{"snapshot",
                                          {4, 4},
                                          gpu::TextureFormat::RGBA8Unorm,
                                          gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc};
  auto first = runtime.createTexture(descriptor);
  ASSERT_FALSE(first.hasError()) << first.error();
  gpu::Texture texture = std::move(first).result();
  gpu::Texture stale =
      gpu::Texture::CreateForBackend(texture.slotIndex(), texture.generation(), texture.deviceId());
  ASSERT_FALSE(runtime.destroyTextureBacking(std::move(texture)).hasError());
  auto replacement = runtime.createTexture(descriptor);
  ASSERT_FALSE(replacement.hasError()) << replacement.error();
  gpu::Texture live = std::move(replacement).result();
  {
    auto snapshot = RendererGeodeTextureSnapshot::AdoptRuntimeTexture(
        sharedDevice(), std::move(stale), {4, 4}, gpu::TextureFormat::RGBA8Unorm,
        AlphaType::Premultiplied);
    EXPECT_THAT(snapshot.isValid(), testing::IsFalse());
    EXPECT_THAT(stale.isValid(), testing::IsTrue());
  }
  EXPECT_THAT(runtime.textureDescriptor(live), gpu::HasResult())
      << "refusing the stale identity must leave the texture now in its slot alone";
  (void)runtime.destroyTextureBacking(std::move(live));
}

/// A registration of another logical context's texture borrows the native allocation. A snapshot
/// may own only its producer's allocation, so it must refuse that registration without consuming
/// the handle or changing the producer's ownership.
TEST_F(RendererGeodeTest, RuntimeSnapshotCannotAdoptABorrowedRegistration) {
  const std::shared_ptr<geode::GeodeDevice> host = sharedDevice();
  ASSERT_THAT(host, testing::NotNull());
  const std::shared_ptr<geode::GeodeDevice> consumer =
      geode::GeodeDevice::CreateOverPhysicalDeviceOwner(host->physicalDeviceOwner(),
                                                        host->textureFormat());
  ASSERT_THAT(consumer, testing::NotNull());
  gpu::Device& producerRuntime = host->runtimeDevice();
  gpu::Device& consumerRuntime = consumer->runtimeDevice();
  auto created =
      producerRuntime.createTexture({"host owner",
                                     {4, 4},
                                     gpu::TextureFormat::RGBA8Unorm,
                                     gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc});
  ASSERT_FALSE(created.hasError()) << created.error();
  gpu::Texture owner = std::move(created).result();
  const gpu::TextureExport exported = gpu::GetResultOrFail(producerRuntime.exportTexture(owner));
  gpu::Texture registration = gpu::GetResultOrFail(consumerRuntime.registerTexture(exported));
  {
    auto snapshot = RendererGeodeTextureSnapshot::AdoptRuntimeTexture(
        consumer, std::move(registration), {4, 4}, gpu::TextureFormat::RGBA8Unorm,
        AlphaType::Premultiplied);
    EXPECT_THAT(snapshot.isValid(), testing::IsFalse());
    EXPECT_THAT(registration.isValid(), testing::IsTrue());
  }
  EXPECT_THAT(producerRuntime.ownsTextureBacking(owner), testing::IsTrue());
  (void)consumerRuntime.destroyTexture(std::move(registration));
  (void)producerRuntime.destroyTextureBacking(std::move(owner));
}

void ExpectSolidRuntimeSnapshot(const RendererBitmap& actual, Vector2i dimensions,
                                const std::array<uint8_t, 4>& color, const char* label) {
  ASSERT_THAT(actual.dimensions, testing::Eq(dimensions));
  RendererBitmap expected;
  expected.dimensions = dimensions;
  expected.rowBytes = actual.rowBytes;
  expected.alphaType = actual.alphaType;
  expected.pixels.resize(expected.rowBytes * dimensions.y);
  for (int y = 0; y < dimensions.y; ++y) {
    for (int x = 0; x < dimensions.x; ++x) {
      std::copy(color.begin(), color.end(),
                expected.pixels.begin() + y * expected.rowBytes + x * 4);
    }
  }
  editor::tests::CompareBitmapToBitmap(actual, expected, label,
                                       editor::tests::PixelmatchIdentityParams());
}

TEST_F(RendererGeodeTest, RuntimeSnapshotMovesPreserveTheResourceIdentity) {
  auto created = sharedDevice()->runtimeDevice().createTexture(
      {"snapshot",
       {4, 4},
       gpu::TextureFormat::RGBA8Unorm,
       gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc});
  ASSERT_FALSE(created.hasError()) << created.error();
  gpu::Texture texture = std::move(created).result();
  const uint32_t slot = texture.slotIndex(), generation = texture.generation();
  auto snapshot = RendererGeodeTextureSnapshot::AdoptRuntimeTexture(
      sharedDevice(), std::move(texture), {2, 3}, gpu::TextureFormat::RGBA8Unorm,
      AlphaType::Premultiplied);
  ASSERT_THAT(snapshot.isValid(), testing::IsTrue());
  EXPECT_THAT(texture.isValid(), testing::IsFalse());
  EXPECT_THAT(snapshot.allocationDimensions(), testing::Eq(Vector2i(4, 4)));
  EXPECT_THAT(snapshot.runtimeFormat(), testing::Optional(gpu::TextureFormat::RGBA8Unorm));
  RendererGeodeTextureSnapshot moved(std::move(snapshot));
  EXPECT_THAT(snapshot.isValid(), testing::IsFalse());
  ASSERT_THAT(moved.runtimeTexture(), testing::NotNull());
  EXPECT_THAT(moved.runtimeTexture()->slotIndex(), testing::Eq(slot));
  EXPECT_THAT(moved.runtimeTexture()->generation(), testing::Eq(generation));
  EXPECT_THAT(moved.deviceId(), testing::Eq(sharedDevice()->runtimeDevice().deviceId()));
  RendererGeodeTextureSnapshot assigned;
  assigned = std::move(moved);
  EXPECT_THAT(moved.isValid(), testing::IsFalse());
  ASSERT_THAT(assigned.runtimeTexture(), testing::NotNull());
  EXPECT_THAT(assigned.runtimeTexture()->slotIndex(), testing::Eq(slot));
  EXPECT_THAT(assigned.setDimensions({4, 4}), testing::IsTrue());
  EXPECT_THAT(assigned.dimensions(), testing::Eq(Vector2i(4, 4)));
}

TEST_F(RendererGeodeTest, BorrowedSnapshotNamesTheSameRuntimeTargetThatDetachmentTransfers) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);
  renderer.endFrame();
  const auto* base = renderer.borrowTextureSnapshot();
  ASSERT_THAT(base, testing::NotNull());
  const auto* borrowed = static_cast<const RendererGeodeTextureSnapshot*>(base);
  ASSERT_THAT(borrowed->runtimeTexture(), testing::NotNull());
  const uint32_t slot = borrowed->runtimeTexture()->slotIndex(),
                 generation = borrowed->runtimeTexture()->generation();
  const auto detached = renderer.takeTextureSnapshot();
  ASSERT_THAT(detached, testing::NotNull());
  const auto* owned = static_cast<const RendererGeodeTextureSnapshot*>(detached.get());
  ASSERT_THAT(owned->runtimeTexture(), testing::NotNull());
  EXPECT_THAT(owned->runtimeTexture()->slotIndex(), testing::Eq(slot));
  EXPECT_THAT(owned->runtimeTexture()->generation(), testing::Eq(generation));
  EXPECT_THAT(renderer.borrowTextureSnapshot(), testing::IsNull());
}

TEST(RuntimeSnapshotLifetime, DetachedContentAndDeviceOutliveTheProducer) {
  auto owner = std::shared_ptr<geode::GeodeDevice>(geode::GeodeDevice::CreateHeadless());
  ASSERT_THAT(owner, testing::NotNull());
  std::weak_ptr<geode::GeodeDevice> lifetime = owner;
  std::shared_ptr<const RendererTextureSnapshot> snapshot;
  {
    RendererGeode renderer(owner);
    RenderViewport viewport;
    viewport.size = {8, 8};
    viewport.devicePixelRatio = 1;
    renderer.beginFrame(viewport);
    renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
    renderer.drawRect(Box2d({0, 0}, {8, 8}), StrokeParams{});
    renderer.endFrame();
    snapshot = renderer.takeTextureSnapshot();
    ASSERT_THAT(snapshot, testing::NotNull());
    renderer.beginFrame(viewport);
    renderer.setPaint(solidFill(css::RGBA(0, 0, 255, 255)));
    renderer.drawRect(Box2d({0, 0}, {8, 8}), StrokeParams{});
    renderer.endFrame();
  }
  owner.reset();
  EXPECT_THAT(lifetime.expired(), testing::IsFalse());
  ExpectSolidRuntimeSnapshot(snapshot->takeSnapshot(), {8, 8}, {255, 0, 0, 255},
                             "runtime_snapshot_retained_content");
  snapshot.reset();
  EXPECT_THAT(lifetime.expired(), testing::IsTrue());
}

TEST_F(RendererGeodeTest, RuntimeSnapshotContentAlphaAndBothByteFormatsRemainExact) {
  for (bool bgra : {false, true}) {
    for (bool straight : {false, true}) {
      SCOPED_TRACE(testing::Message() << "bgra=" << bgra << " straight=" << straight);
      gpu::Device& runtime = sharedDevice()->runtimeDevice();
      auto created = runtime.createTexture(
          {"snapshot",
           {4, 4},
           bgra ? gpu::TextureFormat::BGRA8Unorm : gpu::TextureFormat::RGBA8Unorm,
           gpu::TextureUsage::CopyDst | gpu::TextureUsage::CopySrc | gpu::TextureUsage::Sampled});
      ASSERT_FALSE(created.hasError()) << created.error();
      gpu::Texture texture = std::move(created).result();
      std::array<uint8_t, 1024> pixels{};
      for (size_t y = 0; y < 4; ++y) {
        for (size_t x = 0; x < 4; ++x) {
          const bool content = x < 2 && y < 3;
          const uint8_t red = straight ? 128 : 64;
          const std::array<uint8_t, 4> color = content
                                                   ? (bgra ? std::array<uint8_t, 4>{0, 0, red, 128}
                                                           : std::array<uint8_t, 4>{red, 0, 0, 128})
                                                   : std::array<uint8_t, 4>{0, 255, 0, 255};
          std::copy(color.begin(), color.end(), pixels.begin() + y * 256 + x * 4);
        }
      }
      ASSERT_FALSE(runtime.writeTexture(texture, pixels, {0, 256, 4}, {4, 4}).hasError());
      auto snapshot = RendererGeodeTextureSnapshot::AdoptRuntimeTexture(
          sharedDevice(), std::move(texture), {2, 3},
          bgra ? gpu::TextureFormat::BGRA8Unorm : gpu::TextureFormat::RGBA8Unorm,
          straight ? AlphaType::Unpremultiplied : AlphaType::Premultiplied);
      ASSERT_THAT(snapshot.isValid(), testing::IsTrue());
      ExpectSolidRuntimeSnapshot(snapshot.takeSnapshot(), {2, 3}, {128, 0, 0, 128},
                                 "runtime_snapshot_readback_formats");
      RendererGeode renderer = createRenderer();
      beginFrame(renderer);
      EXPECT_THAT(renderer.drawTextureSnapshot(
                      snapshot, Box2d({0, 0}, {kViewportSize, kViewportSize}), 1, true),
                  testing::IsTrue());
      renderer.endFrame();
      ExpectSolidRuntimeSnapshot(renderer.takeSnapshot(), {64, 64}, {128, 0, 0, 128},
                                 "runtime_snapshot_content_sampling");
    }
  }
}

std::shared_ptr<geode::GeodeDevice> CreateSharedBackendContext(
    const std::shared_ptr<geode::GeodeDevice>& device);

#if defined(__linux__)
/// An owning 4x4 snapshot of a texture \p owner creates, sampleable and copyable.
/// @param owner Device the texture belongs to.
RendererGeodeTextureSnapshot MakeOwnedSnapshot(const std::shared_ptr<geode::GeodeDevice>& owner) {
  gpu::Texture texture = gpu::GetResultOrFail(owner->runtimeDevice().createTexture(
      {"snapshot",
       {4, 4},
       gpu::TextureFormat::RGBA8Unorm,
       gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc}));
  return RendererGeodeTextureSnapshot::AdoptRuntimeTexture(
      owner, std::move(texture), {4, 4}, gpu::TextureFormat::RGBA8Unorm, AlphaType::Premultiplied);
}

/// Two separately selected Vulkan roots own different VkDevice objects, so a snapshot exported
/// from one must be refused by the other. A sibling logical context over the renderer's root is
/// the positive control: it registers and draws the same kind of snapshot.
TEST_F(RendererGeodeTest, ForeignRuntimeSnapshotIsRejectedBeforeRecording) {
  const std::shared_ptr<geode::GeodeDevice> owner = geode::GeodeDevice::CreateHeadless();
  ASSERT_THAT(owner, testing::NotNull());
  const RendererGeodeTextureSnapshot foreign = MakeOwnedSnapshot(owner);
  ASSERT_THAT(foreign.isValid(), testing::IsTrue());
  EXPECT_THAT(
      sharedDevice()->runtimeDevice().registerTexture(
          gpu::GetResultOrFail(owner->runtimeDevice().exportTexture(*foreign.runtimeTexture()))),
      gpu::IsGpuErrorWithMessage(gpu::GpuErrorType::DeviceMismatch,
                                 testing::HasSubstr("belongs to a different native device")));

  const std::shared_ptr<geode::GeodeDevice> sibling = CreateSharedBackendContext(sharedDevice());
  ASSERT_THAT(sibling, testing::NotNull());
  const RendererGeodeTextureSnapshot registrable = MakeOwnedSnapshot(sibling);
  ASSERT_THAT(registrable.isValid(), testing::IsTrue());

  RendererGeode renderer = createRenderer();
  beginFrame(renderer);
  EXPECT_THAT(renderer.drawTextureSnapshot(registrable, Box2d({0, 0}, {4, 4}), 1, true),
              testing::IsTrue())
      << "an identical snapshot made on a sibling device, which registration accepts, must "
         "draw, so the owner is the only thing the renderer refuses";
  EXPECT_THAT(renderer.drawTextureSnapshot(foreign, Box2d({0, 0}, {4, 4}), 1, true),
              testing::IsFalse());
  renderer.endFrame();
}
#endif

TEST_F(RendererGeodeTest, RuntimeReadbackPreservesSampledOnlyAndCopyOnlyRoutes) {
  for (bool sampled : {false, true}) {
    SCOPED_TRACE(sampled);
    gpu::Device& runtime = sharedDevice()->runtimeDevice();
    auto created = runtime.createTexture(
        {"snapshot capability",
         {4, 4},
         gpu::TextureFormat::RGBA8Unorm,
         gpu::TextureUsage::CopyDst |
             (sampled ? gpu::TextureUsage::Sampled : gpu::TextureUsage::CopySrc)});
    ASSERT_FALSE(created.hasError()) << created.error();
    gpu::Texture texture = std::move(created).result();
    std::array<uint8_t, 1024> pixels{};
    for (size_t y = 0; y < 4; ++y) {
      for (size_t x = 0; x < 4; ++x) {
        pixels[y * 256 + x * 4 + 1] = 255;
        pixels[y * 256 + x * 4 + 3] = 255;
      }
    }
    ASSERT_FALSE(runtime.writeTexture(texture, pixels, {0, 256, 4}, {4, 4}).hasError());
    auto snapshot = RendererGeodeTextureSnapshot::AdoptRuntimeTexture(
        sharedDevice(), std::move(texture), {4, 4}, gpu::TextureFormat::RGBA8Unorm,
        AlphaType::Premultiplied);
    ASSERT_THAT(snapshot.isValid(), testing::IsTrue());
    ExpectSolidRuntimeSnapshot(snapshot.takeSnapshot(), {4, 4}, {0, 255, 0, 255},
                               "runtime_snapshot_capability_routes");
    RendererGeode renderer = createRenderer();
    beginFrame(renderer);
    EXPECT_THAT(renderer.drawTextureSnapshot(snapshot, Box2d({0, 0}, {4, 4}), 1, true),
                testing::Eq(sampled));
    renderer.endFrame();
  }
}

TEST_F(RendererGeodeTest, RuntimeSnapshotRejectsUnusableCapabilitiesWithoutConsumption) {
  gpu::Device& runtime = sharedDevice()->runtimeDevice();
  auto created = runtime.createTexture(
      {"upload only", {4, 4}, gpu::TextureFormat::RGBA8Unorm, gpu::TextureUsage::CopyDst});
  ASSERT_FALSE(created.hasError()) << created.error();
  gpu::Texture texture = std::move(created).result();
  {
    auto snapshot = RendererGeodeTextureSnapshot::AdoptRuntimeTexture(
        sharedDevice(), std::move(texture), {4, 4}, gpu::TextureFormat::RGBA8Unorm,
        AlphaType::Premultiplied);
    EXPECT_THAT(snapshot.isValid(), testing::IsFalse());
    EXPECT_THAT(texture.isValid(), testing::IsTrue());
  }
  if (texture.isValid()) {
    (void)runtime.destroyTextureBacking(std::move(texture));
  }
}

TEST_F(RendererGeodeTest, RuntimeSnapshotMoveAssignmentReleasesThePreviousBacking) {
  gpu::Device& runtime = sharedDevice()->runtimeDevice();
  const gpu::TextureDescriptor descriptor{
      "snapshot", {4, 4}, gpu::TextureFormat::RGBA8Unorm, gpu::TextureUsage::Sampled};
  auto first = runtime.createTexture(descriptor);
  auto second = runtime.createTexture(descriptor);
  ASSERT_FALSE(first.hasError()) << first.error();
  ASSERT_FALSE(second.hasError()) << second.error();
  gpu::Texture oldIdentity = gpu::Texture::CreateForBackend(
      first.result().slotIndex(), first.result().generation(), first.result().deviceId());
  auto destination = RendererGeodeTextureSnapshot::AdoptRuntimeTexture(
      sharedDevice(), std::move(first).result(), {4, 4}, gpu::TextureFormat::RGBA8Unorm,
      AlphaType::Premultiplied);
  auto source = RendererGeodeTextureSnapshot::AdoptRuntimeTexture(
      sharedDevice(), std::move(second).result(), {4, 4}, gpu::TextureFormat::RGBA8Unorm,
      AlphaType::Premultiplied);
  destination = std::move(source);
  EXPECT_THAT(source.isValid(), testing::IsFalse());
  EXPECT_THAT(runtime.textureDescriptor(oldIdentity), gpu::HasResult())
      << "the replaced backing must stay live until the owner's drain retires it";
  sharedDevice()->drainDeferredTextureBackings();
  EXPECT_THAT(runtime.textureDescriptor(oldIdentity),
              gpu::IsGpuError(gpu::GpuErrorType::InvalidHandle))
      << "the owner's drain must retire the replaced backing";
  ASSERT_THAT(destination.runtimeTexture(), testing::NotNull());
  EXPECT_THAT(runtime.ownsTextureBacking(*destination.runtimeTexture()), testing::IsTrue());
}

TEST_F(RendererGeodeTest, BorrowedRuntimeSnapshotCanBeDrawnWithoutReleasingTheProducerTarget) {
  RendererGeode producer = createRenderer();
  beginFrame(producer);
  producer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  producer.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});
  producer.endFrame();
  const auto* borrowed = producer.borrowTextureSnapshot();
  ASSERT_THAT(borrowed, testing::NotNull());
  {
    RendererGeode consumer = createRenderer();
    beginFrame(consumer);
    EXPECT_THAT(consumer.drawTextureSnapshot(
                    *borrowed, Box2d({0, 0}, {kViewportSize, kViewportSize}), 1, true),
                testing::IsTrue());
    consumer.endFrame();
    ExpectSolidRuntimeSnapshot(consumer.takeSnapshot(), {64, 64}, {255, 0, 0, 255},
                               "borrowed_runtime_snapshot_drawing");
  }
  const auto detached = producer.takeTextureSnapshot();
  ASSERT_THAT(detached, testing::NotNull());
  EXPECT_THAT(producer.borrowTextureSnapshot(), testing::IsNull());
  ExpectSolidRuntimeSnapshot(detached->takeSnapshot(), {64, 64}, {255, 0, 0, 255},
                             "borrowed_runtime_snapshot_preserves_owner");
  beginFrame(producer);
  producer.endFrame();
  ASSERT_THAT(producer.borrowTextureSnapshot(), testing::NotNull());
  ExpectSolidRuntimeSnapshot(detached->takeSnapshot(), {64, 64}, {255, 0, 0, 255},
                             "borrowed_runtime_snapshot_next_frame");
}

TEST_F(RendererGeodeTest, RuntimeSnapshotBackingSurvivesUntilConsumerSubmission) {
  RendererGeode consumer = createRenderer();
  beginFrame(consumer);
  gpu::Texture sourceIdentity;
  {
    RendererGeode producer = createRenderer();
    beginFrame(producer);
    producer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
    producer.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});
    producer.endFrame();
    const auto snapshot = producer.takeTextureSnapshot();
    ASSERT_THAT(snapshot, testing::NotNull());
    const auto* runtime =
        static_cast<const RendererGeodeTextureSnapshot&>(*snapshot).runtimeTexture();
    ASSERT_THAT(runtime, testing::NotNull());
    sourceIdentity = gpu::Texture::CreateForBackend(runtime->slotIndex(), runtime->generation(),
                                                    runtime->deviceId());
    EXPECT_THAT(consumer.drawTextureSnapshot(
                    *snapshot, Box2d({0, 0}, {kViewportSize, kViewportSize}), 1, true),
                testing::IsTrue());
  }
  EXPECT_THAT(sharedDevice()->runtimeDevice().ownsTextureBacking(sourceIdentity),
              testing::IsTrue());
  consumer.endFrame();
  EXPECT_THAT(sharedDevice()->runtimeDevice().ownsTextureBacking(sourceIdentity),
              testing::IsFalse());
  ExpectSolidRuntimeSnapshot(consumer.takeSnapshot(), {64, 64}, {255, 0, 0, 255},
                             "runtime_snapshot_pending_submission");
}

TEST_F(RendererGeodeTest, UploadedSnapshotBackingSurvivesUntilConsumerSubmission) {
  RendererGeode consumer = createRenderer();
  beginFrame(consumer);
  {
    auto created = sharedDevice()->runtimeDevice().createTexture(
        {"uploaded snapshot",
         {4, 4},
         gpu::TextureFormat::RGBA8Unorm,
         gpu::TextureUsage::Sampled | gpu::TextureUsage::CopyDst});
    ASSERT_FALSE(created.hasError()) << created.error();
    gpu::Texture texture = std::move(created).result();
    // Laid out on the runtime's row pitch, which is wider than the four texels each row holds.
    std::array<uint8_t, 1024> pixels{};
    for (size_t y = 0; y < 4; ++y) {
      for (size_t x = 0; x < 4; ++x) {
        pixels[y * 256 + x * 4] = 255;
        pixels[y * 256 + x * 4 + 3] = 255;
      }
    }
    const gpu::Status written =
        sharedDevice()->runtimeDevice().writeTexture(texture, pixels, {0, 256, 4}, {4, 4});
    ASSERT_FALSE(written.hasError()) << written.error();
    auto snapshot = RendererGeodeTextureSnapshot::AdoptRuntimeTexture(
        sharedDevice(), std::move(texture), {4, 4}, gpu::TextureFormat::RGBA8Unorm,
        AlphaType::Premultiplied);
    ASSERT_THAT(snapshot.isValid(), testing::IsTrue());
    for (int draw = 0; draw < 100; ++draw) {
      EXPECT_THAT(consumer.drawTextureSnapshot(
                      snapshot, Box2d({0, 0}, {kViewportSize, kViewportSize}), 1, true),
                  testing::IsTrue());
    }
  }
  consumer.endFrame();
  ExpectSolidRuntimeSnapshot(consumer.takeSnapshot(), {64, 64}, {255, 0, 0, 255},
                             "uploaded_snapshot_pending_submission");
}

TEST_F(RendererGeodeTest, RuntimeSnapshotBackingIsReleasedWhenConsumerFrameIsDiscarded) {
  gpu::Device& runtime = sharedDevice()->runtimeDevice();
  RendererGeode consumer = createRenderer();
  beginFrame(consumer);
  gpu::Texture sourceIdentity;
  {
    auto created = runtime.createTexture(
        {"snapshot", {4, 4}, gpu::TextureFormat::RGBA8Unorm, gpu::TextureUsage::Sampled});
    ASSERT_FALSE(created.hasError()) << created.error();
    gpu::Texture source = std::move(created).result();
    sourceIdentity =
        gpu::Texture::CreateForBackend(source.slotIndex(), source.generation(), source.deviceId());
    const auto snapshot = RendererGeodeTextureSnapshot::AdoptRuntimeTexture(
        sharedDevice(), std::move(source), {4, 4}, gpu::TextureFormat::RGBA8Unorm,
        AlphaType::Premultiplied);
    EXPECT_THAT(consumer.drawTextureSnapshot(snapshot, Box2d({0, 0}, {4, 4}), 1, true),
                testing::IsTrue());
  }
  EXPECT_THAT(runtime.ownsTextureBacking(sourceIdentity), testing::IsTrue());
  beginFrame(consumer);
  EXPECT_THAT(runtime.ownsTextureBacking(sourceIdentity), testing::IsFalse());
  consumer.endFrame();
  ExpectSolidRuntimeSnapshot(consumer.takeSnapshot(), {64, 64}, {0, 0, 0, 0},
                             "runtime_snapshot_discarded_frame");
}

/// A logical context of its own over the backend root \p device renders through: a separate
/// runtime device on the same backend, whichever backend that is.
std::shared_ptr<geode::GeodeDevice> CreateSharedBackendContext(
    const std::shared_ptr<geode::GeodeDevice>& device) {
  return geode::GeodeDevice::CreateOverPhysicalDeviceOwner(device->physicalDeviceOwner(),
                                                           device->textureFormat());
}

TEST_F(RendererGeodeTest, SharedBackendSnapshotPreservesIdentityAndCroppedContent) {
  auto producer = CreateSharedBackendContext(sharedDevice());
  auto consumer = CreateSharedBackendContext(sharedDevice());
  ASSERT_THAT(producer, testing::NotNull());
  ASSERT_THAT(consumer, testing::NotNull());
  EXPECT_THAT(producer->runtimeDevice().deviceId(),
              testing::Ne(consumer->runtimeDevice().deviceId()));
  auto created = producer->runtimeDevice().createTexture(
      {"shared snapshot",
       {4, 4},
       gpu::TextureFormat::RGBA8Unorm,
       gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc | gpu::TextureUsage::CopyDst});
  ASSERT_FALSE(created.hasError()) << created.error();
  gpu::Texture source = std::move(created).result();
  const uint32_t slot = source.slotIndex();
  const uint32_t generation = source.generation();
  std::array<uint8_t, 1024> pixels{};
  for (size_t y = 0; y < 4; ++y) {
    for (size_t x = 0; x < 4; ++x) {
      pixels[y * 256 + x * 4 + (x < 2 && y < 3 ? 0 : 1)] = 255;
      pixels[y * 256 + x * 4 + 3] = 255;
    }
  }
  ASSERT_FALSE(
      producer->runtimeDevice().writeTexture(source, pixels, {0, 256, 4}, {4, 4}).hasError());
  auto snapshot = RendererGeodeTextureSnapshot::AdoptRuntimeTexture(
      producer, std::move(source), {2, 3}, gpu::TextureFormat::RGBA8Unorm,
      AlphaType::Premultiplied);
  RendererGeode renderer(consumer);
  beginFrame(renderer);
  for (int draw = 0; draw < 100; ++draw) {
    EXPECT_THAT(renderer.drawTextureSnapshot(snapshot, Box2d({0, 0}, {64, 64}), 1, true),
                testing::IsTrue());
  }
  ASSERT_THAT(snapshot.runtimeTexture(), testing::NotNull());
  EXPECT_THAT(snapshot.runtimeTexture()->slotIndex(), testing::Eq(slot));
  EXPECT_THAT(snapshot.runtimeTexture()->generation(), testing::Eq(generation));
  EXPECT_THAT(snapshot.deviceId(), testing::Eq(producer->runtimeDevice().deviceId()));
  renderer.endFrame();
  ExpectSolidRuntimeSnapshot(renderer.takeSnapshot(), {64, 64}, {255, 0, 0, 255},
                             "shared_backend_snapshot_cropped_content");
}

TEST_F(RendererGeodeTest, SharedBackendSnapshotNeedsARegistrationNotMatchingBackendHandles) {
  auto producer = CreateSharedBackendContext(sharedDevice());
  auto consumer = CreateSharedBackendContext(sharedDevice());
  ASSERT_THAT(producer, testing::NotNull());
  ASSERT_THAT(consumer, testing::NotNull());
  ASSERT_THAT(&producer->physicalDeviceOwner()->root(),
              testing::Eq(&consumer->physicalDeviceOwner()->root()))
      << "the two contexts must render through one backend root";
  auto created = producer->runtimeDevice().createTexture(
      {"unregistered snapshot",
       {4, 4},
       gpu::TextureFormat::RGBA8Unorm,
       gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc});
  ASSERT_FALSE(created.hasError()) << created.error();
  auto snapshot = RendererGeodeTextureSnapshot::AdoptRuntimeTexture(
      producer, std::move(created).result(), {4, 4}, gpu::TextureFormat::RGBA8Unorm,
      AlphaType::Premultiplied);
  // A second snapshot of the same shape on the same producer, left intact, is the control: the
  // consumer admits it, so the refusal below is about the registration and not about the context.
  auto control = producer->runtimeDevice().createTexture(
      {"registered snapshot",
       {4, 4},
       gpu::TextureFormat::RGBA8Unorm,
       gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc});
  ASSERT_FALSE(control.hasError()) << control.error();
  auto registeredSnapshot = RendererGeodeTextureSnapshot::AdoptRuntimeTexture(
      producer, std::move(control).result(), {4, 4}, gpu::TextureFormat::RGBA8Unorm,
      AlphaType::Premultiplied);

  // The texture and its registration stay alive here, so the consumer is refused for the one
  // reason under test: the snapshot it was handed no longer names them.
  gpu::Texture registration = snapshot.takeRuntimeRegistrationForTesting();
  ASSERT_THAT(registration.isValid(), testing::IsTrue());

  RendererGeode renderer(consumer);
  beginFrame(renderer);
  EXPECT_THAT(renderer.drawTextureSnapshot(registeredSnapshot, Box2d({0, 0}, {64, 64}), 1, true),
              testing::IsTrue())
      << "The consuming context shares the producer's backend, so a snapshot that still names its "
         "texture must be admitted";
  EXPECT_THAT(renderer.drawTextureSnapshot(snapshot, Box2d({0, 0}, {64, 64}), 1, true),
              testing::IsFalse())
      << "A snapshot that no longer names a texture must be refused, even though its producer "
         "renders through the consuming context's own backend root";
  renderer.endFrame();
  (void)producer->runtimeDevice().destroyTextureBacking(std::move(registration));
}

TEST_F(RendererGeodeTest, SharedBackendSnapshotSurvivesProducerScopeUntilConsumerSubmission) {
  auto producer = CreateSharedBackendContext(sharedDevice());
  auto consumer = CreateSharedBackendContext(sharedDevice());
  ASSERT_THAT(producer, testing::NotNull());
  ASSERT_THAT(consumer, testing::NotNull());
  std::weak_ptr<geode::GeodeDevice> producerLifetime = producer;
  RendererGeode renderer(consumer);
  beginFrame(renderer);
  {
    RendererGeode sourceRenderer(producer);
    beginFrame(sourceRenderer);
    sourceRenderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
    sourceRenderer.drawRect(Box2d({0, 0}, {64, 64}), StrokeParams{});
    sourceRenderer.endFrame();
    const auto snapshot = sourceRenderer.takeTextureSnapshot();
    ASSERT_THAT(snapshot, testing::NotNull());
    EXPECT_THAT(renderer.drawTextureSnapshot(*snapshot, Box2d({0, 0}, {64, 64}), 1, true),
                testing::IsTrue());
  }
  producer.reset();
  EXPECT_THAT(producerLifetime.expired(), testing::IsFalse());
  renderer.endFrame();
  EXPECT_THAT(producerLifetime.expired(), testing::IsTrue());
  ExpectSolidRuntimeSnapshot(renderer.takeSnapshot(), {64, 64}, {255, 0, 0, 255},
                             "shared_backend_snapshot_producer_scope");
}

TEST_F(RendererGeodeTest, RuntimeSnapshotReleaseOnAnotherThreadDefersOwnerSlotRetirement) {
  auto owner = sharedDevice();
  owner->drainDeferredDestroys();
  const size_t pendingBefore = owner->deferredTextureDestroyCountForTesting();
  auto created = owner->runtimeDevice().createTexture(
      {"threaded snapshot", {4, 4}, gpu::TextureFormat::RGBA8Unorm, gpu::TextureUsage::Sampled});
  ASSERT_FALSE(created.hasError()) << created.error();
  gpu::Texture source = std::move(created).result();
  gpu::Texture identity =
      gpu::Texture::CreateForBackend(source.slotIndex(), source.generation(), source.deviceId());
  auto snapshot = std::make_shared<RendererGeodeTextureSnapshot>(
      RendererGeodeTextureSnapshot::AdoptRuntimeTexture(owner, std::move(source), {4, 4},
                                                        gpu::TextureFormat::RGBA8Unorm,
                                                        AlphaType::Premultiplied));
  std::thread releaser([snapshot = std::move(snapshot)]() mutable { snapshot.reset(); });
  releaser.join();
  EXPECT_THAT(owner->runtimeDevice().ownsTextureBacking(identity), testing::IsTrue());
  EXPECT_THAT(owner->deferredTextureDestroyCountForTesting(), testing::Eq(pendingBefore + 1));
  owner->drainDeferredDestroys();
  EXPECT_THAT(owner->runtimeDevice().ownsTextureBacking(identity), testing::IsFalse());
  EXPECT_THAT(owner->deferredTextureDestroyCountForTesting(), testing::Eq(pendingBefore));
}

TEST_F(RendererGeodeTest, SharedBackendPresentationRequiresAnOwningSnapshotLease) {
  RendererGeode producer = createRenderer();
  beginFrame(producer);
  producer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  producer.drawRect(Box2d({0, 0}, {64, 64}), StrokeParams{});
  producer.endFrame();
  const auto* borrowed = producer.borrowTextureSnapshot();
  ASSERT_THAT(borrowed, testing::NotNull());
  auto consumerDevice = CreateSharedBackendContext(sharedDevice());
  ASSERT_THAT(consumerDevice, testing::NotNull());
  RendererGeode consumer(consumerDevice);
  beginFrame(consumer);
  EXPECT_THAT(consumer.drawTextureSnapshot(*borrowed, Box2d({0, 0}, {64, 64}), 1, true),
              testing::IsFalse());
  consumer.endFrame();
  ExpectSolidRuntimeSnapshot(consumer.takeSnapshot(), {64, 64}, {0, 0, 0, 0},
                             "shared_backend_borrowed_snapshot_refused");
  const auto owned = producer.takeTextureSnapshot();
  ASSERT_THAT(owned, testing::NotNull());
  beginFrame(consumer);
  EXPECT_THAT(consumer.drawTextureSnapshot(*owned, Box2d({0, 0}, {64, 64}), 1, true),
              testing::IsTrue());
  consumer.endFrame();
  ExpectSolidRuntimeSnapshot(consumer.takeSnapshot(), {64, 64}, {255, 0, 0, 255},
                             "shared_backend_owned_snapshot_accepted");
}

TEST_F(RendererGeodeTest, DetachedSnapshotReadbackDoesNotMutateProducerFrameCounters) {
  RendererGeode producer = createRenderer();
  beginFrame(producer);
  producer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  producer.drawRect(Box2d({0, 0}, {64, 64}), StrokeParams{});
  producer.endFrame();
  const auto snapshot = producer.takeTextureSnapshot();
  ASSERT_THAT(snapshot, testing::NotNull());
  beginFrame(producer);
  const geode::GeodeCounters before = producer.lastFrameTimings().counters;
  ExpectSolidRuntimeSnapshot(snapshot->takeSnapshot(), {64, 64}, {255, 0, 0, 255},
                             "isolated_snapshot_readback");
  const geode::GeodeCounters after = producer.lastFrameTimings().counters;
  EXPECT_THAT(after.bufferCreates, testing::Eq(before.bufferCreates));
  EXPECT_THAT(after.textureCreates, testing::Eq(before.textureCreates));
  EXPECT_THAT(after.bindgroupCreates, testing::Eq(before.bindgroupCreates));
  EXPECT_THAT(after.submits, testing::Eq(before.submits));
  producer.endFrame();
}

TEST_F(RendererGeodeTest, IsolatedReadbackReusesOneContextAndAccountsForItsPool) {
  auto device = CreateSharedBackendContext(sharedDevice());
  ASSERT_THAT(device, testing::NotNull());
  RendererGeode producer(device);
  beginFrame(producer);
  producer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  producer.drawRect(Box2d({0, 0}, {64, 64}), StrokeParams{});
  producer.endFrame();
  const auto snapshot = producer.takeTextureSnapshot();
  ASSERT_THAT(snapshot, testing::NotNull());
  const uint64_t buffersBefore = device->lifetimeBufferCreates();
  const uint64_t texturesBefore = device->lifetimeTextureCreates();
  ExpectSolidRuntimeSnapshot(snapshot->takeSnapshot(), {64, 64}, {255, 0, 0, 255},
                             "isolated_readback_cold");
  const auto cold = device->consumeReadbackStats();
  EXPECT_THAT(cold.contextCreates, testing::Eq(1u));
  EXPECT_THAT(cold.bufferCreates, testing::Eq(1u));
  EXPECT_THAT(cold.textureCreates, testing::Eq(1u));
  EXPECT_THAT(cold.count, testing::Eq(1));
  EXPECT_THAT(cold.submits, testing::Eq(1u));
  EXPECT_THAT(cold.poolEntries, testing::Eq(1u));
  EXPECT_THAT(cold.poolBytes, testing::Eq(32768u));
  EXPECT_THAT(device->lifetimeBufferCreates(), testing::Eq(buffersBefore + 1));
  EXPECT_THAT(device->lifetimeTextureCreates(), testing::Eq(texturesBefore + 1));
  ExpectSolidRuntimeSnapshot(snapshot->takeSnapshot(), {64, 64}, {255, 0, 0, 255},
                             "isolated_readback_warm");
  const auto warm = device->consumeReadbackStats();
  EXPECT_THAT(warm.contextCreates, testing::Eq(0u));
  EXPECT_THAT(warm.bufferCreates, testing::Eq(0u));
  EXPECT_THAT(warm.textureCreates, testing::Eq(0u));
  EXPECT_THAT(warm.count, testing::Eq(1));
  EXPECT_THAT(warm.poolBytes, testing::Eq(cold.poolBytes));
}

TEST_F(RendererGeodeTest, IsolatedReadbackDeadlineBeforeAcquisitionAllocatesNothing) {
  auto device = CreateSharedBackendContext(sharedDevice());
  ASSERT_THAT(device, testing::NotNull());
  RendererGeode renderer(device);
  beginFrame(renderer);
  renderer.endFrame();
  const uint64_t buffersBefore = device->lifetimeBufferCreates();
  const uint64_t texturesBefore = device->lifetimeTextureCreates();
  device->setSnapshotReadbackBudgetForTesting(std::chrono::milliseconds(0));
  EXPECT_THAT(renderer.takeSnapshot().empty(), testing::IsTrue());
  const auto stats = renderer.consumeReadbackStats();
  EXPECT_THAT(stats.captureTimeouts, testing::Eq(1u));
  EXPECT_THAT(stats.contextCreates, testing::Eq(0u));
  EXPECT_THAT(stats.bufferCreates, testing::Eq(0u));
  EXPECT_THAT(stats.textureCreates, testing::Eq(0u));
  EXPECT_THAT(stats.count, testing::Eq(0));
  EXPECT_THAT(stats.deviceLost, testing::IsFalse());
  EXPECT_THAT(device->lifetimeBufferCreates(), testing::Eq(buffersBefore));
  EXPECT_THAT(device->lifetimeTextureCreates(), testing::Eq(texturesBefore));
}

TEST_F(RendererGeodeTest, IsolatedReadbackWaitHonorsCancellationAndDeadlineWithoutDeviceLoss) {
  auto device = CreateSharedBackendContext(sharedDevice());
  ASSERT_THAT(device, testing::NotNull());
  RendererGeode renderer(device);
  beginFrame(renderer);
  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  renderer.drawRect(Box2d({0, 0}, {64, 64}), StrokeParams{});
  renderer.endFrame();
  const auto snapshot = renderer.takeTextureSnapshot();
  ASSERT_THAT(snapshot, testing::NotNull());
  const auto& geodeSnapshot = static_cast<const RendererGeodeTextureSnapshot&>(*snapshot);
  beginFrame(renderer);
  renderer.endFrame();
  // True once the worker's capture holds the readback context, false when its capture returned
  // without ever acquiring one, as it does when the backend cannot register the snapshot's texture
  // on the capture context.
  std::promise<bool> acquiredSignal;
  std::future<bool> acquired = acquiredSignal.get_future();
  std::latch release(1);
  std::atomic<bool> first{true};
  std::atomic<bool> cancelWaiter{false};
  // Set when the worker's capture does not start in time, so the capture gives up and the case can
  // join it and fail rather than wait on it.
  std::atomic<bool> abandonWorker{false};
  device->setSnapshotReadbackHookForTesting([&](geode::GeodeDevice::SnapshotReadbackPhase phase) {
    if (phase == geode::GeodeDevice::SnapshotReadbackPhase::ContextAcquired &&
        first.exchange(false)) {
      acquiredSignal.set_value(true);
      release.wait();
    } else if (phase == geode::GeodeDevice::SnapshotReadbackPhase::WaitingForContext) {
      cancelWaiter.store(true, std::memory_order_relaxed);
    }
  });
  RendererBitmap captured;
  std::thread worker([&] {
    captured = geodeSnapshot.takeSnapshotInterruptibly(
        [&] { return abandonWorker.load(std::memory_order_relaxed); });
    if (first.exchange(false)) {
      acquiredSignal.set_value(false);
    }
  });
  // The first capture on this context also creates its readback context, a native device
  // creation that can take seconds on a slow driver or an instrumented build, so the bound leaves
  // room for it. It only runs out on the failure path, where it still leaves the case time to
  // abandon the capture, fail and let the binary continue inside the 10 s a case may take.
  constexpr std::chrono::seconds kAcquireBound(8);
  if (acquired.wait_for(kAcquireBound) != std::future_status::ready) {
    // Abandoning the capture ends its wait for the readback context, which it polls, so the join
    // returns. It cannot end the context's creation, a native device creation that takes no
    // cancellation: a capture stalled there still holds the join until the per-case watchdog. The
    // failure is recorded first so that case still names the stall.
    ADD_FAILURE() << "the worker's capture neither acquired its readback context nor returned "
                     "within "
                  << kAcquireBound.count() << " s";
    abandonWorker.store(true, std::memory_order_relaxed);
    release.count_down();
    worker.join();
    device->setSnapshotReadbackHookForTesting({});
    return;
  }
  if (!acquired.get()) {
    release.count_down();
    worker.join();
    device->setSnapshotReadbackHookForTesting({});
    FAIL() << "the worker's capture returned without acquiring its readback context, so the "
              "contention this case measures never happened (its snapshot came back "
           << (captured.empty() ? "empty" : "with pixels") << ")";
  }
  device->setSnapshotReadbackBudgetForTesting(std::chrono::milliseconds(1));
  EXPECT_THAT(renderer.takeSnapshot().empty(), testing::IsTrue());
  device->setSnapshotReadbackBudgetForTesting(geode::kReadbackMapTimeout);
  cancelWaiter.store(false, std::memory_order_relaxed);
  EXPECT_THAT(
      renderer
          .takeSnapshotInterruptibly([&] { return cancelWaiter.load(std::memory_order_relaxed); })
          .empty(),
      testing::IsTrue());
  const auto blocked = renderer.consumeReadbackStats();
  EXPECT_THAT(blocked.captureTimeouts, testing::Eq(1u));
  EXPECT_THAT(blocked.captureCancellations, testing::Eq(1u));
  EXPECT_THAT(blocked.contextCreates, testing::Eq(1u));
  EXPECT_THAT(blocked.bufferCreates, testing::Eq(0u));
  EXPECT_THAT(blocked.textureCreates, testing::Eq(0u));
  EXPECT_THAT(blocked.deviceLost, testing::IsFalse());
  release.count_down();
  for (int frame = 0; frame < 4; ++frame) {
    beginFrame(renderer);
    renderer.endFrame();
    EXPECT_THAT(renderer.lastFrameTimings().counters.submits, testing::Eq(1u));
  }
  worker.join();
  device->setSnapshotReadbackHookForTesting({});
  ExpectSolidRuntimeSnapshot(captured, {64, 64}, {255, 0, 0, 255},
                             "isolated_readback_with_active_producer");
  EXPECT_THAT(device->consumeReadbackStats().count, testing::Eq(1));
  ExpectSolidRuntimeSnapshot(snapshot->takeSnapshot(), {64, 64}, {255, 0, 0, 255},
                             "isolated_readback_after_contention_timeout");
  EXPECT_THAT(device->isDeviceLost(), testing::IsFalse());
}

/// A capture that is abandoned while its mapping is still open must not put the readback set back
/// into the pool. Abandoning a pending map leaves the buffer unusable, so pooling it would hand
/// the next capture a buffer whose backing is gone. The deadline path leaves through the same
/// branch as cancellation, so this covers both ways a capture gives up mid-map.
TEST_F(RendererGeodeTest, ACancelledMappingDoesNotReturnItsBufferToTheReadbackPool) {
  auto device = CreateSharedBackendContext(sharedDevice());
  ASSERT_THAT(device, testing::NotNull());
  RendererGeode renderer(device);
  beginFrame(renderer);
  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  renderer.drawRect(Box2d({0, 0}, {64, 64}), StrokeParams{});
  renderer.endFrame();
  ExpectSolidRuntimeSnapshot(renderer.takeSnapshot(), {64, 64}, {255, 0, 0, 255},
                             "cancelled_mapping_pool_warm");
  const auto warm = device->consumeReadbackStats();
  ASSERT_THAT(warm.poolEntries, testing::Eq(1u)) << "the successful capture pools its set";

  // Cancelling from the MapRequested phase stops the capture after the map has been requested and
  // while it is still pending, which is the state the pool must refuse to accept.
  std::atomic<bool> cancel{false};
  device->setSnapshotReadbackHookForTesting([&](geode::GeodeDevice::SnapshotReadbackPhase phase) {
    if (phase == geode::GeodeDevice::SnapshotReadbackPhase::MapRequested) {
      cancel.store(true, std::memory_order_relaxed);
    }
  });
  beginFrame(renderer);
  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  renderer.drawRect(Box2d({0, 0}, {64, 64}), StrokeParams{});
  renderer.endFrame();
  EXPECT_THAT(
      renderer.takeSnapshotInterruptibly([&] { return cancel.load(std::memory_order_relaxed); })
          .empty(),
      testing::IsTrue());
  device->setSnapshotReadbackHookForTesting({});

  const auto cancelled = device->consumeReadbackStats();
  EXPECT_THAT(cancelled.captureCancellations, testing::Eq(1u));
  EXPECT_THAT(cancelled.poolEntries, testing::Eq(0u))
      << "A capture abandoned with its mapping still open must drop its readback set instead of "
         "pooling it for the next caller";
  EXPECT_THAT(device->isDeviceLost(), testing::IsFalse())
      << "Giving up on a capture is not a device failure";
}

/// The pooled readback buffer is mapped again by the next capture, which is only possible because
/// the previous capture released its mapping: one buffer carries one mapping at a time, so a
/// mapping the renderer failed to release would make the second capture's map fail outright.
TEST_F(RendererGeodeTest, APooledReadbackBufferIsMappableAgainOnceItsMappingIsReleased) {
  auto device = CreateSharedBackendContext(sharedDevice());
  ASSERT_THAT(device, testing::NotNull());
  RendererGeode renderer(device);
  beginFrame(renderer);
  renderer.setPaint(solidFill(css::RGBA(0, 0, 255, 255)));
  renderer.drawRect(Box2d({0, 0}, {64, 64}), StrokeParams{});
  renderer.endFrame();
  ExpectSolidRuntimeSnapshot(renderer.takeSnapshot(), {64, 64}, {0, 0, 255, 255},
                             "remapped_pooled_buffer_first");
  const auto first = device->consumeReadbackStats();
  ASSERT_THAT(first.poolEntries, testing::Eq(1u));

  beginFrame(renderer);
  renderer.setPaint(solidFill(css::RGBA(0, 255, 0, 255)));
  renderer.drawRect(Box2d({0, 0}, {64, 64}), StrokeParams{});
  renderer.endFrame();
  ExpectSolidRuntimeSnapshot(renderer.takeSnapshot(), {64, 64}, {0, 255, 0, 255},
                             "remapped_pooled_buffer_second");

  const auto second = device->consumeReadbackStats();
  EXPECT_THAT(second.count, testing::Eq(1));
  EXPECT_THAT(second.bufferCreates, testing::Eq(0u))
      << "The second capture must reuse the pooled buffer, so it is the same buffer being mapped "
         "a second time";
  EXPECT_THAT(second.poolEntries, testing::Eq(1u));
}

TEST_F(RendererGeodeTest, IsolatedReadbackPoolRemainsBoundedAcrossSizes) {
  auto device = CreateSharedBackendContext(sharedDevice());
  ASSERT_THAT(device, testing::NotNull());
  RendererGeode renderer(device);
  for (int size : {8, 16, 24, 32, 40}) {
    RenderViewport viewport;
    viewport.size = Vector2d(size, size);
    viewport.devicePixelRatio = 1;
    renderer.beginFrame(viewport);
    renderer.endFrame();
    ExpectSolidRuntimeSnapshot(renderer.takeSnapshot(), {size, size}, {0, 0, 0, 0},
                               "isolated_readback_pool_sizes");
    const auto stats = device->consumeReadbackStats();
    EXPECT_THAT(stats.poolEntries, testing::Le(4u));
    EXPECT_THAT(stats.poolBytes, testing::Gt(0u));
  }
  const auto retained = device->consumeReadbackStats();
  EXPECT_THAT(retained.poolEntries, testing::Eq(4u));
  EXPECT_THAT(retained.poolBytes, testing::Eq(42496u));
}

/// Copies the `size` rectangle at `origin` out of `bitmap` into a tightly packed bitmap, so content
/// drawn into part of a larger target can be compared with the same content drawn on its own. A
/// rectangle that does not lie inside `bitmap` fails the test and yields an empty bitmap.
RendererBitmap CopyBitmapRegion(const RendererBitmap& bitmap, Vector2i origin, Vector2i size) {
  if (origin.x < 0 || origin.y < 0 || size.x < 0 || size.y < 0 ||
      origin.x + size.x > bitmap.dimensions.x || origin.y + size.y > bitmap.dimensions.y) {
    ADD_FAILURE() << "region at " << origin << " of size " << size << " is outside the "
                  << bitmap.dimensions << " bitmap";
    return RendererBitmap();
  }
  RendererBitmap region;
  region.dimensions = size;
  region.rowBytes = static_cast<std::size_t>(size.x) * 4u;
  region.alphaType = bitmap.alphaType;
  region.pixels.resize(region.rowBytes * static_cast<std::size_t>(size.y));
  for (int y = 0; y < size.y; ++y) {
    const uint8_t* source = bitmap.pixels.data() +
                            static_cast<std::size_t>(origin.y + y) * bitmap.rowBytes +
                            static_cast<std::size_t>(origin.x) * 4u;
    std::copy_n(source, region.rowBytes,
                region.pixels.data() + static_cast<std::size_t>(y) * region.rowBytes);
  }
  return region;
}

/// One fill case for the placement tests: an SVG body drawn over a 16-unit view box, and a pixel
/// of the 20 px render whose center lies exactly on one of its edges.
struct PixelCenterEdgeCase {
  std::string_view name;
  std::string_view body;
  Vector2i edgePixel;
};

/// Each case runs a different fill pipeline: a solo solid fill, a cross-entity batch of two
/// paints, slanted edges, a gradient, a path clip rendered through the mask pipeline, and a
/// pattern. At 20 px over a 16-unit view box (scale 1.25) their edges fall at 2.5 and 17.5, on
/// pixel centers, and the triangle's slanted edges pass through pixel centers as well.
constexpr std::array<PixelCenterEdgeCase, 6> kPixelCenterEdgeCases = {{
    {"solid", R"svg(<rect x="2" y="2" width="12" height="12" fill="#000"/>)svg", {2, 8}},
    {"abutting_paints",
     R"svg(<rect x="2" y="2" width="6" height="12" fill="#000"/>)svg"
     R"svg(<rect x="8" y="2" width="6" height="12" fill="#fff"/>)svg",
     {2, 8}},
    {"slanted_edges", R"svg(<path d="M8 2 L14 14 L2 14 Z" fill="#000"/>)svg", {4, 13}},
    {"linear_gradient",
     R"svg(<defs><linearGradient id="g"><stop offset="0" stop-color="#000"/>)svg"
     R"svg(<stop offset="1" stop-color="#3973ad"/></linearGradient></defs>)svg"
     R"svg(<rect x="2" y="2" width="12" height="12" fill="url(#g)"/>)svg",
     {2, 8}},
    {"clip_mask",
     R"svg(<defs><clipPath id="c"><path d="M8 2 L14 14 L2 14 Z"/></clipPath></defs>)svg"
     R"svg(<rect width="16" height="16" fill="#000" clip-path="url(#c)"/>)svg",
     {4, 13}},
    {"pattern",
     R"svg(<defs><pattern id="p" width="4" height="4" patternUnits="userSpaceOnUse">)svg"
     R"svg(<rect width="4" height="4" fill="#b75d23"/><rect width="2" height="2" fill="#31ba55"/>)svg"
     R"svg(</pattern></defs><rect x="2" y="2" width="12" height="12" fill="url(#p)"/>)svg",
     {2, 8}},
}};

/// Parses one of the placement cases as a 20 px document over a 16-unit view box.
std::optional<SVGDocument> ParsePixelCenterEdgeCase(const PixelCenterEdgeCase& testCase) {
  const std::string source =
      std::string(R"(<svg xmlns="http://www.w3.org/2000/svg" width="20" height="20")"
                  R"( viewBox="0 0 16 16">)") +
      std::string(testCase.body) + "</svg>";
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(source, warnings);
  if (parsed.hasError()) {
    return std::nullopt;
  }
  return std::move(parsed.result());
}

// Coverage at a pixel whose center lies exactly on a path edge or vertex is decided by the sample
// position alone: the half-open ownership rule gives the edge to exactly one side of the ray, and
// a half-covered pixel sits on the 127.5 rounding boundary. That position must not depend on where
// the draw lands in its target or how large the target is, or the same shape renders differently
// in an atlas tile than on its own, which is the contract `RenderDocumentsToAtlasBitmap` offers.
TEST_F(RendererGeodeTest, PixelCenterEdgeCoverageDoesNotDependOnPlacement) {
  struct Placement {
    Vector2i targetSize;
    Vector2i origin;
  };
  constexpr std::array<Placement, 3> kPlacements = {{
      {{97, 53}, {0, 0}},
      {{97, 53}, {41, 17}},
      {{64, 64}, {3, 29}},
  }};
  constexpr Vector2i kTileSize(20, 20);

  for (const PixelCenterEdgeCase& testCase : kPixelCenterEdgeCases) {
    SCOPED_TRACE(testCase.name);
    std::optional<SVGDocument> standaloneDocument = ParsePixelCenterEdgeCase(testCase);
    ASSERT_THAT(standaloneDocument, testing::Optional(testing::_));
    RendererGeode standaloneRenderer = createRenderer();
    standaloneRenderer.draw(*standaloneDocument);
    const RendererBitmap standalone = standaloneRenderer.takeSnapshot();
    ASSERT_EQ(standalone.dimensions, kTileSize);
    const uint8_t edgeAlpha =
        standalone.pixels[static_cast<std::size_t>(testCase.edgePixel.y) * standalone.rowBytes +
                          static_cast<std::size_t>(testCase.edgePixel.x) * 4u + 3u];
    EXPECT_THAT(edgeAlpha, testing::AllOf(testing::Gt(0), testing::Lt(255)))
        << "pixel " << testCase.edgePixel
        << " should be partially covered: its center is on an edge";

    for (const Placement& placement : kPlacements) {
      std::optional<SVGDocument> atlasDocument = ParsePixelCenterEdgeCase(testCase);
      ASSERT_THAT(atlasDocument, testing::Optional(testing::_));
      RendererGeode atlasRenderer = createRenderer();
      const std::array<AtlasDocumentPlacement, 1> placements = {
          {AtlasDocumentPlacement{&*atlasDocument, placement.origin}}};
      const RendererBitmap atlas =
          RenderDocumentsToAtlasBitmap(atlasRenderer, placements, placement.targetSize);
      ASSERT_EQ(atlas.dimensions, placement.targetSize);
      const std::string label =
          std::string("pixel_center_edges_") + std::string(testCase.name) + "_" +
          std::to_string(placement.targetSize.x) + "x" + std::to_string(placement.targetSize.y) +
          "_at_" + std::to_string(placement.origin.x) + "_" + std::to_string(placement.origin.y);
      editor::tests::CompareBitmapToBitmap(CopyBitmapRegion(atlas, placement.origin, kTileSize),
                                           standalone, label,
                                           editor::tests::PixelmatchIdentityParams());
    }
  }
}

/// A draw's target, in device pixels, and where its canvas origin lands in it.
struct DevicePlacement {
  Vector2i targetSize;
  Vector2i origin;
};

/// One document for the placement precision tests, and a pixel of its unplaced render whose
/// alpha shows the document was drawn: partially covered when its center lies on an edge, or
/// fully covered for an interior probe.
struct PlacementPrecisionCase {
  std::string name;
  std::string source;
  Vector2i probePixel;
  bool probeOnEdge;
};

/// Builds an SVG document source of `size` px over `viewBox` around `body`.
std::string PlacementDocument(int size, std::string_view viewBox, std::string_view body) {
  return std::string(R"(<svg xmlns="http://www.w3.org/2000/svg" width=")") + std::to_string(size) +
         R"(" height=")" + std::to_string(size) + R"(" viewBox=")" + std::string(viewBox) +
         R"(">)" + std::string(body) + "</svg>";
}

/// Draws `source` through the renderer driver, the path `RenderDocumentsToAtlasBitmap` takes, into
/// a fresh `targetSize` frame at `devicePixelRatio`, with the canvas mapped to the target by
/// `canvasTransform`, then the device pixel ratio, then a translation to `origin`.
std::optional<RendererBitmap> RenderDocumentPlaced(
    const std::shared_ptr<geode::GeodeDevice>& device, std::string_view source, Vector2i targetSize,
    Vector2i origin, double devicePixelRatio, const Transform2d& canvasTransform) {
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(source, warnings);
  if (parsed.hasError()) {
    return std::nullopt;
  }
  SVGDocument document = std::move(parsed.result());
  RenderViewport viewport;
  viewport.size = Vector2d(targetSize.x / devicePixelRatio, targetSize.y / devicePixelRatio);
  viewport.devicePixelRatio = devicePixelRatio;
  const Transform2d surfaceFromCanvas = canvasTransform * Transform2d::Scale(devicePixelRatio) *
                                        Transform2d::Translate(Vector2d(origin.x, origin.y));
  RendererGeode renderer(device);
  renderer.beginFrame(viewport);
  RendererDriver driver(renderer);
  renderer.setTransform(surfaceFromCanvas);
  driver.drawDocumentIntoCurrentFrame(document, viewport, surfaceFromCanvas);
  renderer.endFrame();
  return renderer.takeSnapshot();
}

/// Renders each case once with its canvas at the origin of a `tileSize` target and once at each
/// placement, and requires every placed tile to match the unplaced render exactly.
void ExpectPlacementExactRenders(const std::shared_ptr<geode::GeodeDevice>& device,
                                 std::string_view label,
                                 std::span<const PlacementPrecisionCase> cases, Vector2i tileSize,
                                 double devicePixelRatio, const Transform2d& canvasTransform,
                                 std::span<const DevicePlacement> placements) {
  for (const PlacementPrecisionCase& testCase : cases) {
    SCOPED_TRACE(testCase.name);
    const std::optional<RendererBitmap> reference = RenderDocumentPlaced(
        device, testCase.source, tileSize, Vector2i::Zero(), devicePixelRatio, canvasTransform);
    ASSERT_THAT(reference, testing::Optional(testing::_));
    ASSERT_EQ(reference->dimensions, tileSize);
    const uint8_t probeAlpha =
        reference->pixels[static_cast<std::size_t>(testCase.probePixel.y) * reference->rowBytes +
                          static_cast<std::size_t>(testCase.probePixel.x) * 4u + 3u];
    if (testCase.probeOnEdge) {
      EXPECT_THAT(probeAlpha, testing::AllOf(testing::Gt(0), testing::Lt(255)))
          << "pixel " << testCase.probePixel << " should be partially covered";
    } else {
      EXPECT_THAT(probeAlpha, testing::Eq(255))
          << "pixel " << testCase.probePixel << " should be covered";
    }

    for (const DevicePlacement& placement : placements) {
      const std::optional<RendererBitmap> placed =
          RenderDocumentPlaced(device, testCase.source, placement.targetSize, placement.origin,
                               devicePixelRatio, canvasTransform);
      ASSERT_THAT(placed, testing::Optional(testing::_));
      ASSERT_EQ(placed->dimensions, placement.targetSize);
      const std::string tileLabel =
          std::string(label) + "_" + testCase.name + "_" + std::to_string(placement.targetSize.x) +
          "x" + std::to_string(placement.targetSize.y) + "_at_" +
          std::to_string(placement.origin.x) + "_" + std::to_string(placement.origin.y);
      editor::tests::CompareBitmapToBitmap(CopyBitmapRegion(*placed, placement.origin, tileSize),
                                           *reference, tileLabel,
                                           editor::tests::PixelmatchIdentityParams());
    }
  }
}

/// The pixel-center cases as full 20 px documents over a 16-unit view box.
std::vector<PlacementPrecisionCase> PixelCenterEdgePrecisionCases() {
  std::vector<PlacementPrecisionCase> cases;
  for (const PixelCenterEdgeCase& testCase : kPixelCenterEdgeCases) {
    cases.push_back({std::string(testCase.name), PlacementDocument(20, "0 0 16 16", testCase.body),
                     testCase.edgePixel, true});
  }
  return cases;
}

// Placement exactness holds for any linear part and any fractional translation, not only for
// the pixel-aligned scales that put edges on pixel centers: the mapping is exact in the integer
// part of the translation and identical in everything else. A rotation about the tile center
// with a non-integer scale gives both a non-axis-aligned linear part and a fractional
// translation.
TEST_F(RendererGeodeTest, PlacementExactCoverageUnderRotationAndFractionalScale) {
  std::vector<PlacementPrecisionCase> cases = PixelCenterEdgePrecisionCases();
  for (PlacementPrecisionCase& testCase : cases) {
    // The tile center stays covered under a rotation about it.
    testCase.probePixel = Vector2i(10, 10);
    testCase.probeOnEdge = false;
  }
  const Transform2d canvasTransform = Transform2d::Translate(Vector2d(-10, -10)) *
                                      Transform2d::Rotate(MathConstants<double>::kPi / 7.0) *
                                      Transform2d::Scale(1.13) *
                                      Transform2d::Translate(Vector2d(10, 10));
  constexpr std::array<DevicePlacement, 2> kPlacements = {{
      {{97, 53}, {41, 17}},
      {{64, 64}, {3, 29}},
  }};
  ExpectPlacementExactRenders(sharedDevice(), "rotated", cases, Vector2i(20, 20), 1.0,
                              canvasTransform, kPlacements);
}

// A device pixel ratio scales the canvas before placement, so exactness holds at DPR 2 as well.
// Quarter-unit coordinates at scale 2 put these edges on pixel centers (4.5, 35.5) and the
// triangle's slanted edges through them.
TEST_F(RendererGeodeTest, PlacementExactCoverageAtDevicePixelRatioTwo) {
  const std::vector<PlacementPrecisionCase> cases = {
      {"solid",
       PlacementDocument(20, "0 0 20 20",
                         R"svg(<rect x="2.25" y="2.25" width="15.5" height="15.5"/>)svg"),
       {4, 20},
       true},
      {"slanted_edges",
       PlacementDocument(20, "0 0 20 20",
                         R"svg(<path d="M10 2.25 L17.75 17.75 L2.25 17.75 Z"/>)svg"),
       {19, 5},
       true},
      {"clip_mask",
       PlacementDocument(20, "0 0 20 20",
                         R"svg(<defs><clipPath id="c"><path d="M10 2.25 L17.75 17.75)svg"
                         R"svg( L2.25 17.75 Z"/></clipPath></defs>)svg"
                         R"svg(<rect width="20" height="20" clip-path="url(#c)"/>)svg"),
       {19, 5},
       true},
  };
  constexpr std::array<DevicePlacement, 2> kPlacements = {{
      {{96, 52}, {41, 11}},
      {{64, 64}, {23, 3}},
  }};
  ExpectPlacementExactRenders(sharedDevice(), "dpr2", cases, Vector2i(40, 40), 2.0, Transform2d(),
                              kPlacements);
}

// Subtracting the integer pixel origin is exact for every target the device can create, so a tile
// placed thousands of pixels into a wide target maps its pixels exactly as one at the origin does.
TEST_F(RendererGeodeTest, PlacementExactCoverageAtLargeDeviceOffsets) {
  constexpr std::array<DevicePlacement, 2> kPlacements = {{
      {{8192, 32}, {8150, 7}},
      {{4200, 64}, {4097, 41}},
  }};
  ExpectPlacementExactRenders(sharedDevice(), "far", PixelCenterEdgePrecisionCases(),
                              Vector2i(20, 20), 1.0, Transform2d(), kPlacements);
}

// Path data far from the path-space origin puts the draw's integer pixel origin far away too; the
// subtraction stays exact below 2^22 pixels, so placement exactness does not depend on how large
// the path coordinates are. These view boxes start at 65536, so the geometry's coordinates are
// 65538 and up.
TEST_F(RendererGeodeTest, PlacementExactCoverageAtLargePathCoordinates) {
  constexpr std::string_view kViewBox = "65536 65536 16 16";
  const std::vector<PlacementPrecisionCase> cases = {
      {"solid",
       PlacementDocument(20, kViewBox,
                         R"svg(<rect x="65538" y="65538" width="12" height="12"/>)svg"),
       {2, 8},
       true},
      {"slanted_edges",
       PlacementDocument(20, kViewBox,
                         R"svg(<path d="M65544 65538 L65550 65550 L65538 65550 Z"/>)svg"),
       {4, 13},
       true},
      {"clip_mask",
       PlacementDocument(20, kViewBox,
                         R"svg(<defs><clipPath id="c"><path d="M65544 65538 L65550 65550)svg"
                         R"svg( L65538 65550 Z"/></clipPath></defs>)svg"
                         R"svg(<rect x="65536" y="65536" width="16" height="16")svg"
                         R"svg( clip-path="url(#c)"/>)svg"),
       {4, 13},
       true},
  };
  constexpr std::array<DevicePlacement, 2> kPlacements = {{
      {{97, 53}, {41, 17}},
      {{64, 64}, {3, 29}},
  }};
  ExpectPlacementExactRenders(sharedDevice(), "large_path", cases, Vector2i(20, 20), 1.0,
                              Transform2d(), kPlacements);
}

// A transform without an inverse maps the path onto a line, which covers no area, so nothing may
// be drawn. The host gives such a draw an all-zero pixel-to-path mapping. The GPU builds the draw's
// enclosure from float32 axes that pick up rounding from the clip-space mapping, so the enclosure
// is not necessarily degenerate and can rasterize a sliver of pixels; the shaders must still cover
// none of them. Each case puts the path-space origin inside the geometry, where a zero mapping
// would otherwise read full coverage, and runs a different pipeline: a solo fill, a cross-entity
// batch of two paints, a gradient, a clip mask and a pattern. Every render must be identical to a
// fully transparent target.
TEST_F(RendererGeodeTest, TransformWithoutInverseDrawsNothing) {
  constexpr std::array<std::string_view, 3> kSingularTransforms = {
      "matrix(1 3 3 9 13 29)",
      "matrix(2 1 4 2 40 20)",
      "matrix(0.25 0.75 0.5 1.5 48.5 26.5)",
  };
  constexpr std::array<std::string_view, 5> kBodies = {
      R"svg(<rect x="-5" y="-5" width="10" height="10" transform="TRANSFORM"/>)svg",
      R"svg(<g transform="TRANSFORM"><rect x="-5" y="-5" width="5" height="10" fill="#000"/>)svg"
      R"svg(<rect x="0" y="-5" width="5" height="10" fill="#3973ad"/></g>)svg",
      R"svg(<defs><linearGradient id="g"><stop offset="0" stop-color="#000"/>)svg"
      R"svg(<stop offset="1" stop-color="#3973ad"/></linearGradient></defs>)svg"
      R"svg(<rect x="-5" y="-5" width="10" height="10" fill="url(#g)" transform="TRANSFORM"/>)svg",
      R"svg(<defs><clipPath id="c"><rect x="-5" y="-5" width="10" height="10")svg"
      R"svg( transform="TRANSFORM"/></clipPath></defs>)svg"
      R"svg(<rect width="97" height="53" fill="#000" clip-path="url(#c)"/>)svg",
      R"svg(<defs><pattern id="p" width="4" height="4" patternUnits="userSpaceOnUse">)svg"
      R"svg(<rect width="4" height="4" fill="#b75d23"/></pattern></defs>)svg"
      R"svg(<rect x="-5" y="-5" width="10" height="10" fill="url(#p)" transform="TRANSFORM"/>)svg",
  };
  constexpr Vector2i kTargetSize(97, 53);

  for (std::size_t transformIndex = 0; transformIndex < kSingularTransforms.size();
       ++transformIndex) {
    for (std::size_t bodyIndex = 0; bodyIndex < kBodies.size(); ++bodyIndex) {
      std::string body(kBodies[bodyIndex]);
      const std::size_t marker = body.find("TRANSFORM");
      ASSERT_NE(marker, std::string::npos);
      body.replace(marker, std::string_view("TRANSFORM").size(),
                   kSingularTransforms[transformIndex]);
      SCOPED_TRACE(body);
      const std::string source =
          R"(<svg xmlns="http://www.w3.org/2000/svg" width="97" height="53">)" + body + "</svg>";
      const std::optional<RendererBitmap> rendered = RenderDocumentPlaced(
          sharedDevice(), source, kTargetSize, Vector2i::Zero(), 1.0, Transform2d());
      ASSERT_THAT(rendered, testing::Optional(testing::_));
      ASSERT_EQ(rendered->dimensions, kTargetSize);

      RendererBitmap transparent;
      transparent.dimensions = rendered->dimensions;
      transparent.rowBytes = rendered->rowBytes;
      transparent.alphaType = rendered->alphaType;
      transparent.pixels.assign(rendered->pixels.size(), 0u);
      editor::tests::CompareBitmapToBitmap(*rendered, transparent,
                                           "transform_without_inverse_" +
                                               std::to_string(transformIndex) + "_" +
                                               std::to_string(bodyIndex),
                                           editor::tests::PixelmatchIdentityParams());
    }
  }
}

/// A document of \p count glowing pieces in a \p width by \p height canvas: each a small group
/// whose filter merges a blur under its source.
std::string GlowingPiecesSource(int width, int height, int count) {
  std::string source = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" +
                       std::to_string(width) + "\" height=\"" + std::to_string(height) + "\">";
  source += R"svg(<filter id="glow" x="-40%" y="-40%" width="180%" height="180%">
      <feGaussianBlur in="SourceGraphic" stdDeviation="4" result="halo"/>
      <feMerge><feMergeNode in="halo"/><feMergeNode in="SourceGraphic"/></feMerge>
    </filter>)svg";
  for (int i = 0; i < count; ++i) {
    const int x = 40 + (i % 9) * 128;
    const int y = 60 + (i / 9) * 280;
    source += "<g filter=\"url(#glow)\"><circle cx=\"" + std::to_string(x) + "\" cy=\"" +
              std::to_string(y) + "\" r=\"30\" fill=\"#e05090\"/><rect x=\"" +
              std::to_string(x - 10) + "\" y=\"" + std::to_string(y - 40) +
              "\" width=\"20\" height=\"16\" fill=\"#40c0f0\"/></g>";
  }
  return source + "</svg>";
}

SVGDocument ParseDocumentOrFail(const std::string& source) {
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(source, warnings);
  EXPECT_FALSE(parsed.hasError());
  return std::move(parsed.result());
}

constexpr int kPhoneWidth = 1206;
constexpr int kPhoneHeight = 2622;
constexpr std::uint64_t kPhoneTargetBytes = std::uint64_t{kPhoneWidth} * kPhoneHeight * 4;

// A small filtered element's capture and intermediates cover its filter region, not the whole
// viewport it sits in.
TEST_F(RendererGeodeTest, SmallFilterInLargeViewportCapturesOnlyItsRegion) {
  SVGDocument document = ParseDocumentOrFail(GlowingPiecesSource(kPhoneWidth, kPhoneHeight, 1));
  RendererGeode renderer = createRenderer();
  renderer.draw(document);
  const RendererResourceStats stats = renderer.resourceStats();
  EXPECT_THAT(stats.filterExecutions, testing::Eq(1u));
  // The 60x70 piece's filter region is 108x126 pixels, so its capture is at most 128x160.
  EXPECT_THAT(stats.filterCaptureBytesReserved, testing::Le(std::uint64_t{128} * 160 * 4));
  EXPECT_THAT(stats.surfacePeakBytes, testing::Lt(kPhoneTargetBytes + (std::uint64_t{2} << 20)))
      << "filter surfaces beyond the frame target";
}

// Many small filters reuse a few pooled surfaces: once warm, a frame creates no textures and holds
// far less than one viewport-sized surface per filter.
TEST_F(RendererGeodeTest, ManySmallFiltersReusePooledSurfacesOnceWarm) {
  constexpr int kPieces = 80;
  SVGDocument document =
      ParseDocumentOrFail(GlowingPiecesSource(kPhoneWidth, kPhoneHeight, kPieces));
  RendererGeode renderer = createRenderer();
  for (int frame = 0; frame < 3; ++frame) {
    renderer.draw(document);
  }
  renderer.draw(document);
  const RendererResourceStats stats = renderer.resourceStats();
  EXPECT_THAT(stats.filterExecutions, testing::Eq(static_cast<std::uint64_t>(kPieces)));
  EXPECT_THAT(renderer.lastFrameTimings().counters.textureCreates, testing::Eq(0u));
  EXPECT_THAT(stats.surfacePeakBytes, testing::Lt(kPhoneTargetBytes + (std::uint64_t{32} << 20)));
}

// Reusing filter textures within a frame changes which texture each filter writes, never what it
// writes.
TEST_F(RendererGeodeTest, WithinFrameTextureReuseMatchesSingleUseTextures) {
  SVGDocument document = ParseDocumentOrFail(GlowingPiecesSource(kPhoneWidth, kPhoneHeight, 80));
  RendererGeode reusing = createRenderer();
  reusing.draw(document);
  reusing.draw(document);
  RendererGeode singleUse = createRenderer();
  singleUse.setWithinFrameTextureReuseForTesting(false);
  singleUse.draw(document);
  editor::tests::CompareBitmapToBitmap(reusing.takeSnapshot(), singleUse.takeSnapshot(),
                                       "within_frame_texture_reuse",
                                       editor::tests::PixelmatchIdentityParams());
}

/// A 256x128 canvas of overlapping sibling circles, circle `i` wrapped in the group element
/// \p openGroup returns for it, with `<mask>`s `m` (the whole canvas) and `m2` (its left half) and
/// `<clipPath>`s `c` and `c2` (a smaller circle on the left) to name.
std::string SiblingGroupsSource(int count, const std::function<std::string(int)>& openGroup) {
  std::string source = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="256" height="128">
      <mask id="m"><rect width="256" height="128" fill="#999999"/></mask>
      <mask id="m2"><rect width="128" height="128" fill="#999999"/></mask>
      <clipPath id="c"><circle cx="128" cy="64" r="60"/></clipPath>
      <clipPath id="c2"><circle cx="64" cy="64" r="40"/></clipPath>
      <rect width="256" height="128" fill="#204080"/>)svg";
  for (int i = 0; i < count; ++i) {
    source += openGroup(i) + "<circle cx=\"" + std::to_string(12 + (i % 12) * 20) + "\" cy=\"" +
              std::to_string(36 + (i / 12) * 56) + "\" r=\"16\" fill=\"" +
              (i % 2 == 0 ? "red" : "lime") + "\"/></g>";
  }
  return source + "</svg>";
}

/// Draws \p source once with surfaces shared within the frame and once with one surface per use,
/// and returns how many more surface bytes the second held at its peak.
std::uint64_t SurfaceBytesSavedBySharing(RendererGeode& reusing, RendererGeode& singleUse,
                                         const std::string& source) {
  SVGDocument document = ParseDocumentOrFail(source);
  reusing.draw(document);
  singleUse.setWithinFrameTextureReuseForTesting(false);
  singleUse.draw(document);
  const RendererResourceStats reusingStats = reusing.resourceStats();
  const RendererResourceStats singleUseStats = singleUse.resourceStats();
  EXPECT_THAT(reusingStats.surfaceBudgetSupported, testing::IsTrue());
  EXPECT_THAT(singleUseStats.surfacePeakBytes, testing::Ge(reusingStats.surfacePeakBytes));
  return singleUseStats.surfacePeakBytes - reusingStats.surfacePeakBytes;
}

constexpr std::uint64_t kSiblingSurfaceBytes = std::uint64_t{256} * 128 * 4;

// Sibling layers take turns with one canvas-sized layer surface: a layer's surface is free for the
// next once its composite is recorded. A blend layer's snapshot of what lies beneath it waits for
// the frame's submission, so each blend layer still adds one.
TEST_F(RendererGeodeTest, SiblingLayersShareOneLayerSurface) {
  constexpr int kNormalLayers = 20;
  constexpr int kBlendLayers = 4;
  RendererGeode reusing = createRenderer();
  RendererGeode singleUse = createRenderer();
  EXPECT_THAT(
      SurfaceBytesSavedBySharing(
          reusing, singleUse,
          SiblingGroupsSource(kNormalLayers + kBlendLayers,
                              [](int i) {
                                return std::string(i < kNormalLayers
                                                       ? R"(<g opacity="0.7">)"
                                                       : R"(<g style="mix-blend-mode: multiply">)");
                              })),
      testing::Eq((kNormalLayers + kBlendLayers - 1) * kSiblingSurfaceBytes));
}

// Sibling masked groups take turns with one mask and one content surface.
TEST_F(RendererGeodeTest, SiblingMasksShareTwoSurfaces) {
  constexpr int kMasks = 12;
  RendererGeode reusing = createRenderer();
  RendererGeode singleUse = createRenderer();
  EXPECT_THAT(
      SurfaceBytesSavedBySharing(
          reusing, singleUse,
          SiblingGroupsSource(kMasks, [](int) { return std::string("<g mask=\"url(#m)\">"); })),
      testing::Eq((2 * kMasks - 2) * kSiblingSurfaceBytes));
}

// Sibling path clips take turns with one clip-mask surface.
TEST_F(RendererGeodeTest, SiblingClipPathsShareOneClipMask) {
  constexpr int kClips = 12;
  RendererGeode reusing = createRenderer();
  RendererGeode singleUse = createRenderer();
  EXPECT_THAT(SurfaceBytesSavedBySharing(
                  reusing, singleUse,
                  SiblingGroupsSource(
                      kClips, [](int) { return std::string("<g clip-path=\"url(#c)\">"); })),
              testing::Eq((kClips - 1) * kSiblingSurfaceBytes));
}

// Layers, masks and clips that take turns with surfaces render what they render with a surface
// each, also when they nest, when a filter takes turns with them, and on a frame whose surfaces
// come back from the pool holding the previous frame's pixels. Consecutive masks and clips differ,
// so a surface that kept its previous user's pixels would show.
TEST_F(RendererGeodeTest, SharedSurfacesMatchOneSurfacePerUse) {
  std::string source = SiblingGroupsSource(15, [](int i) {
    const bool second = (i / 5) % 2 == 1;
    switch (i % 5) {
      case 0: return std::string(R"(<g style="mix-blend-mode: multiply">)");
      case 1: return std::string(second ? "<g mask=\"url(#m2)\">" : "<g mask=\"url(#m)\">");
      case 2:
        return std::string(second ? "<g clip-path=\"url(#c2)\">" : "<g clip-path=\"url(#c)\">");
      default: return std::string(R"(<g opacity="0.7">)");
    }
  });
  source.erase(source.size() - std::string_view("</svg>").size());
  source += R"svg(
      <filter id="f"><feOffset dx="3" dy="2"/></filter>
      <g mask="url(#m)">
        <g opacity="0.5"><rect x="20" y="70" width="60" height="40" fill="orange"/></g>
        <g clip-path="url(#c)"><rect x="40" y="80" width="60" height="40" fill="white"/></g>
      </g>
      <g filter="url(#f)">
        <g opacity="0.6"><rect x="110" y="70" width="50" height="40" fill="purple"/></g>
      </g>
      <g opacity="0.8">
        <g opacity="0.5"><rect x="170" y="70" width="60" height="40" fill="lime"/></g>
        <g clip-path="url(#c)" opacity="0.5">
          <rect x="190" y="80" width="40" height="40" fill="red"/>
        </g>
      </g>
      <g opacity="0.9"><rect x="200" y="10" width="50" height="50" fill="yellow"/></g>
    </svg>)svg";
  SVGDocument document = ParseDocumentOrFail(source);
  RendererGeode reusing = createRenderer();
  reusing.draw(document);
  reusing.draw(document);
  RendererGeode singleUse = createRenderer();
  singleUse.setWithinFrameTextureReuseForTesting(false);
  singleUse.draw(document);
  ASSERT_THAT(reusing.resourceStats().surfacePeakBytes,
              testing::Lt(singleUse.resourceStats().surfacePeakBytes))
      << "the scene's surfaces are not shared, so this compares nothing new";
  editor::tests::CompareBitmapToBitmap(reusing.takeSnapshot(), singleUse.takeSnapshot(),
                                       "shared_surfaces",
                                       editor::tests::PixelmatchIdentityParams());
}

// A full-canvas blur on a canvas larger than 2048x2048 renders, and reusing its textures within
// the frame matches single-use textures. On this backend the blur executes in tiles below the
// shared pool's budget, so this guards the large-raster path rather than reproducing a texture
// destroyed while pending; ManyDifferentlySizedFiltersInOneFrameSubmit covers that.
TEST_F(RendererGeodeTest, FullCanvasBlurBeyondThePoolBudgetRenders) {
  const std::string source =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="2100" height="2100">
      <filter id="soft" x="0" y="0" width="1" height="1"><feGaussianBlur stdDeviation="6"/></filter>
      <g filter="url(#soft)"><rect width="2100" height="2100" fill="#3070c0"/>
        <rect x="1000" y="1000" width="100" height="100" fill="#ffffff"/></g>
    </svg>)svg";
  SVGDocument document = ParseDocumentOrFail(source);
  RendererGeode reusing = createRenderer();
  reusing.draw(document);
  ASSERT_THAT(reusing.deviceLost(), testing::IsFalse());
  const RendererBitmap actual = reusing.takeSnapshot();
  ASSERT_FALSE(actual.empty());
  EXPECT_THAT(PixelAt(actual, 400, 400), RgbaEq(48, 112, 192, 255));
  RendererGeode singleUse = createRenderer();
  singleUse.setWithinFrameTextureReuseForTesting(false);
  singleUse.draw(document);
  editor::tests::CompareBitmapToBitmap(actual, singleUse.takeSnapshot(), "full_canvas_blur_reuse",
                                       editor::tests::PixelmatchIdentityParams());
}

// Filters of many different sizes in one frame release far more intermediates than the shared
// pool can hold. A frame that handed them to the pool as they were released let it evict, and so
// destroy, textures the frame's pending commands still named, and the frame's submission failed
// and lost the device. They now wait for the frame to submit.
TEST_F(RendererGeodeTest, ManyDifferentlySizedFiltersInOneFrameSubmit) {
  std::string source = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" +
                       std::to_string(kPhoneWidth) + "\" height=\"" + std::to_string(kPhoneHeight) +
                       "\">";
  source += R"svg(<filter id="glow" x="-40%" y="-40%" width="180%" height="180%">
      <feGaussianBlur in="SourceGraphic" stdDeviation="6" result="halo"/>
      <feMerge><feMergeNode in="halo"/><feMergeNode in="SourceGraphic"/></feMerge>
    </filter>)svg";
  for (int i = 0; i < 40; ++i) {
    const int radius = 12 + i * 9;
    source += "<g filter=\"url(#glow)\"><circle cx=\"" + std::to_string(150 + (i % 4) * 300) +
              "\" cy=\"" + std::to_string(200 + (i / 4) * 240) + "\" r=\"" +
              std::to_string(radius) + "\" fill=\"#e05090\"/></g>";
  }
  source += "</svg>";
  SVGDocument document = ParseDocumentOrFail(source);
  std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
  ASSERT_THAT(device, testing::NotNull());
  RendererGeode renderer(device);
  renderer.draw(document);
  ASSERT_THAT(renderer.deviceLost(), testing::IsFalse());
  const std::uint64_t executions = renderer.resourceStats().filterExecutions;
  EXPECT_THAT(executions, testing::Gt(0u));
  const RendererBitmap actual = renderer.takeSnapshot();
  ASSERT_FALSE(actual.empty());
  RendererGeode singleUse(device);
  singleUse.setWithinFrameTextureReuseForTesting(false);
  singleUse.draw(document);
  ASSERT_THAT(singleUse.deviceLost(), testing::IsFalse());
  EXPECT_THAT(singleUse.resourceStats().filterExecutions, testing::Eq(executions))
      << "reuse must not change which filters the frame budget admits";
  editor::tests::CompareBitmapToBitmap(actual, singleUse.takeSnapshot(),
                                       "differently_sized_filters_submit",
                                       editor::tests::PixelmatchIdentityParams());
}

/// A document of 120 glowing circles of 40 sizes on the phone-sized canvas, whose filters hold more
/// region-sized surfaces until the frame ends than the pool's starting budget, but less than its
/// native ceiling.
std::string VariedGlowSource() {
  std::string source = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" +
                       std::to_string(kPhoneWidth) + "\" height=\"" + std::to_string(kPhoneHeight) +
                       "\">";
  source += R"svg(<filter id="glow" x="-40%" y="-40%" width="180%" height="180%">
      <feGaussianBlur in="SourceGraphic" stdDeviation="5" result="halo"/>
      <feMerge><feMergeNode in="halo"/><feMergeNode in="SourceGraphic"/></feMerge>
    </filter>)svg";
  for (int i = 0; i < 120; ++i) {
    source += "<g filter=\"url(#glow)\"><circle cx=\"" + std::to_string(100 + (i % 8) * 140) +
              "\" cy=\"" + std::to_string(100 + (i / 8) * 165) + "\" r=\"" +
              std::to_string(40 + (i % 40) * 2) + "\" fill=\"#e05090\"/></g>";
  }
  return source + "</svg>";
}

constexpr std::uint64_t kPoolFloorBytes = std::uint64_t{64} << 20;

// Filtered elements of many different sizes need more than the pool's starting budget. Once warm,
// the pool keeps that whole working set from frame to frame instead of destroying and recreating
// most of it every frame. This holds while the working set stays under the pool's ceiling.
TEST_F(RendererGeodeTest, VariedFilterSizesCreateNoTexturesOnceWarm) {
  SVGDocument document = ParseDocumentOrFail(VariedGlowSource());
  std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
  ASSERT_THAT(device, testing::NotNull());
  RendererGeode renderer(device);
  for (int frame = 0; frame < 3; ++frame) {
    renderer.draw(document);
  }
  renderer.draw(document);
  ASSERT_THAT(renderer.deviceLost(), testing::IsFalse());
  EXPECT_THAT(renderer.resourceStats().filterExecutions, testing::Gt(0u));
  EXPECT_THAT(renderer.lastFrameTimings().counters.textureCreates, testing::Eq(0u));
  const RendererGeodeTexturePoolStats pool = renderer.texturePoolStats();
  EXPECT_THAT(pool.budgetBytes, testing::Gt(kPoolFloorBytes));
  EXPECT_THAT(pool.budgetBytes, testing::Le(pool.maximumBudgetBytes));
}

// A bucket left unused for longer than the pool keeps idle buckets is destroyed whole. The pool's
// texture count and bytes must both drop by everything it held, so a trim to nothing leaves both
// at zero.
TEST_F(RendererGeodeTest, StaleBucketEvictionKeepsThePoolTotalsConsistent) {
  RendererGeode renderer = createRenderer();
  const auto frameWithNestedLayers = [&](double width, int layers) {
    RenderViewport viewport;
    viewport.size = Vector2d(width, kViewportSize);
    viewport.devicePixelRatio = 1.0;
    renderer.beginFrame(viewport);
    for (int layer = 0; layer < layers; ++layer) {
      renderer.pushIsolatedLayer(0.5, MixBlendMode::Normal);
    }
    for (int layer = 0; layer < layers; ++layer) {
      renderer.popIsolatedLayer();
    }
    renderer.endFrame();
  };
  frameWithNestedLayers(96.0, 3);
  ASSERT_THAT(renderer.texturePoolStats().textureCount, testing::Ge(3u));

  // Frames of another size leave the first size's bucket idle well past the stale-bucket age.
  for (int frame = 0; frame < 130; ++frame) {
    frameWithNestedLayers(kViewportSize, 0);
  }
  renderer.trimTexturePool(RendererGeodeTexturePoolTrim::All);

  const RendererGeodeTexturePoolStats stats = renderer.texturePoolStats();
  EXPECT_THAT(stats.textureCount, testing::Eq(0u));
  EXPECT_THAT(stats.bytes, testing::Eq(0u));
}

// An embedder can give the pool's free textures back: to the floor when rendering goes idle, and
// all of them when the app is hidden.
TEST_F(RendererGeodeTest, TrimTexturePoolFreesFreeTextures) {
  SVGDocument document = ParseDocumentOrFail(VariedGlowSource());
  std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
  ASSERT_THAT(device, testing::NotNull());
  RendererGeode renderer(device);
  for (int frame = 0; frame < 3; ++frame) {
    renderer.draw(document);
  }
  ASSERT_THAT(renderer.texturePoolStats().bytes, testing::Gt(kPoolFloorBytes));

  renderer.trimTexturePool(RendererGeodeTexturePoolTrim::ToFloor);
  EXPECT_THAT(renderer.texturePoolStats().bytes, testing::Le(kPoolFloorBytes));
  EXPECT_THAT(renderer.texturePoolStats().budgetBytes, testing::Eq(kPoolFloorBytes));

  renderer.trimTexturePool(RendererGeodeTexturePoolTrim::All);
  EXPECT_THAT(renderer.texturePoolStats().bytes, testing::Eq(0u));
  EXPECT_THAT(renderer.texturePoolStats().textureCount, testing::Eq(0u));
}

// A trim forgets the heavy frame's working set even when rendering resumes before the idle decay:
// the textures that frame returned after it submitted must not raise the budget again at the next
// frame.
TEST_F(RendererGeodeTest, TrimTexturePoolForgetsTheLastFrameReleases) {
  SVGDocument heavy = ParseDocumentOrFail(VariedGlowSource());
  SVGDocument light = ParseDocumentOrFail(GlowingPiecesSource(64, 64, 0));
  for (const RendererGeodeTexturePoolTrim level :
       {RendererGeodeTexturePoolTrim::ToFloor, RendererGeodeTexturePoolTrim::All}) {
    SCOPED_TRACE(level == RendererGeodeTexturePoolTrim::All ? "All" : "ToFloor");
    std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
    ASSERT_THAT(device, testing::NotNull());
    RendererGeode renderer(device);
    for (int frame = 0; frame < 3; ++frame) {
      renderer.draw(heavy);
    }
    ASSERT_THAT(renderer.texturePoolStats().budgetBytes, testing::Gt(kPoolFloorBytes));

    renderer.trimTexturePool(level);
    renderer.draw(light);
    ASSERT_THAT(renderer.deviceLost(), testing::IsFalse());
    EXPECT_THAT(renderer.texturePoolStats().budgetBytes, testing::Eq(kPoolFloorBytes));
    EXPECT_THAT(renderer.texturePoolStats().bytes, testing::Le(kPoolFloorBytes));
  }
}

// A device that stops rendering forgets its working set after two seconds, whether the next
// event is a frame or an idle trim, so a scene rendered once does not keep its peak forever.
TEST_F(RendererGeodeTest, IdleTexturePoolReturnsToItsFloor) {
  SVGDocument heavy = ParseDocumentOrFail(VariedGlowSource());
  SVGDocument light = ParseDocumentOrFail(GlowingPiecesSource(64, 64, 0));
  for (const bool viaFrame : {false, true}) {
    SCOPED_TRACE(viaFrame ? "next frame" : "idle trim");
    // Declared first so it outlives the pool whose clock reads it.
    auto now = std::chrono::steady_clock::time_point();
    std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
    ASSERT_THAT(device, testing::NotNull());
    RendererGeode renderer(device);
    renderer.setTexturePoolClockForTesting([&now] { return now; });
    for (int frame = 0; frame < 3; ++frame) {
      now += std::chrono::milliseconds(16);
      renderer.draw(heavy);
    }
    ASSERT_THAT(renderer.texturePoolStats().budgetBytes, testing::Gt(kPoolFloorBytes));

    now += std::chrono::seconds(1);
    renderer.trimTexturePool(RendererGeodeTexturePoolTrim::IdleOnly);
    EXPECT_THAT(renderer.texturePoolStats().budgetBytes, testing::Gt(kPoolFloorBytes))
        << "a second of idle keeps the working set";

    now += std::chrono::seconds(2);
    if (viaFrame) {
      renderer.draw(light);
    } else {
      renderer.trimTexturePool(RendererGeodeTexturePoolTrim::IdleOnly);
    }
    EXPECT_THAT(renderer.texturePoolStats().budgetBytes, testing::Eq(kPoolFloorBytes));
    EXPECT_THAT(renderer.texturePoolStats().bytes, testing::Le(kPoolFloorBytes));
  }
}

// A texture parked for reuse within the frame is still live memory, so it stays charged to the
// frame's surface budget until the frame ends: right after a filter pops, nothing it allocated has
// been released from the budget.
TEST_F(RendererGeodeTest, ParkedFilterTexturesStayChargedUntilTheFrameEnds) {
  using namespace components;
  FilterGraph graph;
  FilterNode blur;
  blur.primitive = filter_primitive::GaussianBlur{.stdDeviationX = 3, .stdDeviationY = 3};
  graph.nodes.push_back(blur);
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);
  renderer.pushFilterLayer(graph, Box2d({0, 0}, {kViewportSize, kViewportSize}));
  renderer.setPaint(solidFill(css::RGBA(200, 40, 40, 255)));
  renderer.drawRect(Box2d({16, 16}, {48, 48}), StrokeParams{});
  renderer.popFilterLayer();
  const RendererResourceStats stats = renderer.resourceStats();
  EXPECT_THAT(stats.surfaceBytes, testing::Eq(stats.surfacePeakBytes));
  renderer.endFrame();
}

// A frame that takes longer than the idle decay is activity, not idleness: the textures it returns
// when it ends keep the device's working set for the frame after it.
TEST_F(RendererGeodeTest, SlowFrameKeepsTheTexturePoolWorkingSet) {
  using namespace components;
  SVGDocument heavy = ParseDocumentOrFail(VariedGlowSource());
  // Declared first so it outlives the pool whose clock reads it.
  auto now = std::chrono::steady_clock::time_point();
  std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
  ASSERT_THAT(device, testing::NotNull());
  RendererGeode renderer(device);
  renderer.setTexturePoolClockForTesting([&now] { return now; });
  for (int frame = 0; frame < 3; ++frame) {
    now += std::chrono::milliseconds(16);
    renderer.draw(heavy);
  }
  ASSERT_THAT(renderer.texturePoolStats().budgetBytes, testing::Gt(kPoolFloorBytes));

  FilterGraph graph;
  FilterNode blur;
  blur.primitive = filter_primitive::GaussianBlur{.stdDeviationX = 3, .stdDeviationY = 3};
  graph.nodes.push_back(blur);
  beginFrame(renderer);
  now += std::chrono::seconds(3);  // The frame itself takes three seconds.
  renderer.pushFilterLayer(graph, Box2d({0, 0}, {kViewportSize, kViewportSize}));
  renderer.setPaint(solidFill(css::RGBA(200, 40, 40, 255)));
  renderer.drawRect(Box2d({16, 16}, {48, 48}), StrokeParams{});
  renderer.popFilterLayer();
  renderer.endFrame();

  now += std::chrono::milliseconds(100);
  renderer.draw(heavy);
  EXPECT_THAT(renderer.texturePoolStats().budgetBytes, testing::Gt(kPoolFloorBytes));
}

// Textures one renderer releases within an open frame stay with it: another renderer on the same
// device could otherwise take one, write it, and submit ahead of the first renderer's frame.
TEST_F(RendererGeodeTest, WithinFrameReleasesStayWithTheirRenderer) {
  using namespace components;
  FilterGraph graph;
  FilterNode blur;
  blur.primitive = filter_primitive::GaussianBlur{.stdDeviationX = 3, .stdDeviationY = 3};
  graph.nodes.push_back(blur);
  const auto drawFilter = [&](RendererGeode& renderer, const css::RGBA& color) {
    renderer.pushFilterLayer(graph, Box2d({0, 0}, {kViewportSize, kViewportSize}));
    renderer.setPaint(solidFill(color));
    renderer.drawRect(Box2d({16, 16}, {48, 48}), StrokeParams{});
    renderer.popFilterLayer();
  };

  // A device of their own, so the shared pool starts empty and only these renderers feed it.
  std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
  ASSERT_THAT(device, testing::NotNull());
  RendererGeode first(device);
  RendererGeode second(device);
  beginFrame(first);
  const std::size_t pooledBefore = first.texturePoolStats().textureCount;
  drawFilter(first, css::RGBA(200, 40, 40, 255));
  EXPECT_THAT(first.texturePoolStats().textureCount, testing::Le(pooledBefore))
      << "an open frame's released filter textures reached the shared pool";

  beginFrame(second);
  drawFilter(second, css::RGBA(40, 200, 40, 255));
  second.endFrame();

  drawFilter(first, css::RGBA(200, 40, 40, 255));
  first.endFrame();

  RendererGeode reference = createRenderer();
  beginFrame(reference);
  drawFilter(reference, css::RGBA(200, 40, 40, 255));
  drawFilter(reference, css::RGBA(200, 40, 40, 255));
  reference.endFrame();
  editor::tests::CompareBitmapToBitmap(first.takeSnapshot(), reference.takeSnapshot(),
                                       "within_frame_release_isolation",
                                       editor::tests::PixelmatchIdentityParams());
}

/// Renders \p content in a \p size canvas, and again in a canvas with \p room more pixels on
/// every side and the content moved by \p room, then requires the window of the second that
/// shows the first canvas to match it: a filter near or past the canvas edge must read and show
/// the same pixels as one with room around it.
void ExpectFilterEdgeParity(RendererGeode& renderer, const std::string& content, int size, int room,
                            std::string_view label,
                            const editor::tests::BitmapGoldenCompareParams& params =
                                editor::tests::PixelmatchIdentityParams()) {
  const std::string filters = R"svg(
    <filter id="glow" x="-40%" y="-40%" width="180%" height="180%">
      <feGaussianBlur in="SourceGraphic" stdDeviation="5" result="halo"/>
      <feMerge><feMergeNode in="halo"/><feMergeNode in="SourceGraphic"/></feMerge>
    </filter>
    <filter id="wide" x="-100%" y="-100%" width="300%" height="300%">
      <feGaussianBlur stdDeviation="9"/>
    </filter>
    <filter id="stretch" x="-60%" y="-60%" width="220%" height="220%">
      <feGaussianBlur stdDeviation="1 7"/>
    </filter>
    <filter id="shift" x="-60%" y="-60%" width="220%" height="220%">
      <feOffset dx="-14" dy="-11"/>
    </filter>
    <filter id="sharpen">
      <feConvolveMatrix order="3" kernelMatrix="0 -1 0 -1 5 -1 0 -1 0"/>
    </filter>
    <filter id="tiles" filterUnits="userSpaceOnUse" x="-40" y="-40" width="200" height="200">
      <feOffset x="-10" y="-10" width="16" height="16" result="cell"/>
      <feTile in="cell"/>
    </filter>
    <filter id="spot" x="-30%" y="-30%" width="160%" height="160%">
      <feDiffuseLighting in="SourceAlpha" surfaceScale="3" lighting-color="#ffe0a0">
        <fePointLight x="2" y="4" z="20"/>
      </feDiffuseLighting>
      <feComposite in2="SourceGraphic" operator="in"/>
    </filter>
    <clipPath id="clip"><rect x="-6" y="-6" width="40" height="34"/></clipPath>
    <mask id="fade"><rect x="-40" y="-40" width="200" height="200" fill="#808080"/></mask>)svg";
  const auto canvas = [&](int extent, int shift) {
    return "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" + std::to_string(extent) +
           "\" height=\"" + std::to_string(extent) + "\">" + filters + "<g transform=\"translate(" +
           std::to_string(shift) + " " + std::to_string(shift) + ")\">" + content + "</g></svg>";
  };
  SVGDocument atEdge = ParseDocumentOrFail(canvas(size, 0));
  renderer.draw(atEdge);
  const RendererBitmap edgeBitmap = renderer.takeSnapshot();
  SVGDocument withRoom = ParseDocumentOrFail(canvas(size + 2 * room, room));
  renderer.draw(withRoom);
  const RendererBitmap roomBitmap = renderer.takeSnapshot();
  ASSERT_FALSE(edgeBitmap.empty());
  ASSERT_FALSE(roomBitmap.empty());
  editor::tests::CompareBitmapToBitmap(
      edgeBitmap, editor::tests::CropBitmap(roomBitmap, Vector2i(room, room), Vector2i(size, size)),
      label, params);
}

TEST_F(RendererGeodeTest, FilterRegionsNearCanvasEdgesMatchFiltersWithRoom) {
  RendererGeode renderer = createRenderer();
  const std::array<std::pair<std::string_view, std::string_view>, 13> cases{{
      {"straddles_top_left",
       R"svg(<g filter="url(#glow)"><circle cx="6" cy="10" r="18" fill="#e05090"/></g>)svg"},
      {"straddles_bottom_right",
       R"svg(<g filter="url(#glow)"><rect x="80" y="84" width="30" height="24" fill="#40c0f0"/></g>)svg"},
      {"halo_only_past_edge",
       R"svg(<g filter="url(#wide)"><rect x="-30" y="30" width="24" height="24" fill="#208040"/></g>)svg"},
      {"rotated",
       R"svg(<g transform="translate(90 20) rotate(30)" filter="url(#glow)"><rect x="-20" y="-10" width="40" height="20" fill="#e0a020"/></g>)svg"},
      {"scaled",
       R"svg(<g transform="translate(-10 50) scale(2.5)" filter="url(#glow)"><circle cx="10" cy="6" r="8" fill="#6040e0"/></g>)svg"},
      {"negative_offset_top_left",
       R"svg(<g filter="url(#shift)"><rect x="4" y="2" width="30" height="26" fill="#30a050"/></g>)svg"},
      {"convolve_duplicate_edges",
       R"svg(<g filter="url(#sharpen)"><rect x="-8" y="70" width="40" height="40" fill="#5070c0"/></g>)svg"},
      {"tile_user_space_source",
       R"svg(<g filter="url(#tiles)"><circle cx="-4" cy="-4" r="7" fill="#c03080"/></g>)svg"},
      {"point_light_near_edge",
       R"svg(<g filter="url(#spot)"><rect x="-4" y="-2" width="36" height="30" fill="#a07030"/></g>)svg"},
      {"nested_filters",
       R"svg(<g filter="url(#glow)"><g filter="url(#shift)"><rect x="6" y="8" width="24" height="20" fill="#2080c0"/></g></g>)svg"},
      {"under_ancestor_clip",
       R"svg(<g clip-path="url(#clip)"><g filter="url(#wide)"><rect x="-6" y="0" width="30" height="20" fill="#d04040"/></g></g>)svg"},
      {"under_opacity",
       R"svg(<g opacity="0.6"><g filter="url(#glow)"><circle cx="90" cy="88" r="14" fill="#40a040"/></g></g>)svg"},
      {"inside_mask",
       R"svg(<g mask="url(#fade)"><g filter="url(#glow)"><rect x="70" y="-6" width="30" height="24" fill="#8040c0"/></g></g>)svg"},
  }};
  for (const auto& [label, content] : cases) {
    SCOPED_TRACE(label);
    ExpectFilterEdgeParity(renderer, std::string(content), 96, 64,
                           "filter_edge_parity_" + std::string(label));
  }
}

// A filtered group inside a pattern tile, away from the tile origin, captures a raster over its
// own filter region, so its children must be drawn moved into that raster just as outside a tile.
// A tile that covers the canvas one to one shows the same pixels as the group drawn directly, also
// when a filter outside the tile, whose capture does not hold the tile's content, encloses both.
// The filters only move and merge pixels of pixel-aligned rectangles, so the tile and the direct
// draw match exactly.
TEST_F(RendererGeodeTest, FilterInsidePatternTileMatchesTheFilterDrawnDirectly) {
  RendererGeode renderer = createRenderer();
  const std::string filters = R"svg(
    <filter id="echo" x="-50%" y="-50%" width="200%" height="200%">
      <feOffset dx="7" dy="5" result="moved"/>
      <feMerge><feMergeNode in="moved"/><feMergeNode in="SourceGraphic"/></feMerge>
    </filter>
    <filter id="shift" filterUnits="userSpaceOnUse" x="-20" y="-20" width="136" height="136">
      <feOffset dx="14" dy="11"/>
    </filter>)svg";
  const std::string group =
      R"svg(<g filter="url(#echo)"><rect x="54" y="50" width="20" height="20" fill="#e05090"/>)svg"
      R"svg(<rect x="54" y="40" width="20" height="8" fill="#40c0f0"/></g>)svg";
  const std::string tiled =
      R"svg(<defs><pattern id="p" patternUnits="userSpaceOnUse" width="96" height="96">)svg" +
      group + R"svg(</pattern></defs><rect width="96" height="96" fill="url(#p)"/>)svg";
  const auto canvas = [&](const std::string& content) {
    return R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="96" height="96">)svg" + filters +
           content + "</svg>";
  };
  struct Wrapper {
    std::string_view label;
    std::string_view attributes;
    /// Where the outer filter moves the group's content.
    Vector2i shift;
  };
  const std::array<Wrapper, 2> wrappers{{
      {"alone", "", Vector2i(0, 0)},
      {"inside_outer_filter", R"svg(filter="url(#shift)")svg", Vector2i(14, 11)},
  }};
  for (const auto& [label, wrapper, shift] : wrappers) {
    SCOPED_TRACE(label);
    const auto wrap = [&](const std::string& content) {
      return "<g " + std::string(wrapper) + ">" + content + "</g>";
    };
    SVGDocument patternDocument = ParseDocumentOrFail(canvas(wrap(tiled)));
    renderer.draw(patternDocument);
    const RendererBitmap patternBitmap = renderer.takeSnapshot();
    SVGDocument directDocument = ParseDocumentOrFail(canvas(wrap(group)));
    renderer.draw(directDocument);
    const RendererBitmap directBitmap = renderer.takeSnapshot();
    ASSERT_FALSE(patternBitmap.empty());
    ASSERT_FALSE(directBitmap.empty());
    // The direct render holds the echo's moved copy where only that copy reaches, so two renders
    // that both lost the filtered content cannot match vacuously.
    EXPECT_THAT(PixelAt(directBitmap, 78 + shift.x, 72 + shift.y), RgbaEq(224, 80, 144, 255));
    editor::tests::CompareBitmapToBitmap(patternBitmap, directBitmap,
                                         "filter_inside_pattern_tile_" + std::string(label),
                                         editor::tests::PixelmatchIdentityParams());
  }
}

// A rotated anisotropic blur runs on the transformed path, which resamples the capture into a
// local raster aligned with the filter's axes. That resampling is not exactly invariant to where
// the canvas edge falls: some edge pixels of the halo differ slightly, which they also did when
// filter surfaces spanned the viewport, so they are held to the suite's default per-pixel
// threshold rather than to identity.
TEST_F(RendererGeodeTest, RotatedAnisotropicFilterNearCanvasEdgeMatchesFilterWithRoom) {
  RendererGeode renderer = createRenderer();
  ExpectFilterEdgeParity(
      renderer,
      R"svg(<g transform="translate(8 60) rotate(40)" filter="url(#stretch)"><rect x="-14" y="-14" width="28" height="28" fill="#c02040"/></g>)svg",
      96, 64, "filter_edge_parity_rotated_anisotropic",
      editor::tests::ApprovedPixelToleranceParams(0.02f, 0, /*includeAntiAliasing=*/true));
}

// Marker placements after the first draw their content from a record of their own over the
// content's resident geometry, and the clipped ones apply their marker viewport in the fragment
// stage rather than through the scissor. The reference draws every placement through a marker of
// its own, placed once, so each content entity takes the ordinary first-draw path. Every edge sits
// on a pixel boundary, so the two must agree exactly, and the overlaps (stroke under the markers,
// each placement over the previous one, later elements over the markers) pin painter order on both
// the first frame and the steady frame.
TEST_F(RendererGeodeTest, MarkerPlacementsMatchSinglePlacementMarkersInPainterOrder) {
  // `#clipped` keeps the default `overflow: hidden`; its green square spills out of the marker
  // viewport and is cut by it. `#loose` is unclipped, and each placement's magenta covers the cyan
  // edge of the one before it.
  const auto clippedMarker = [](const std::string& id) {
    return R"svg(<marker id=")svg" + id +
           R"svg(" markerWidth="12" markerHeight="12" refX="2" refY="2"
               markerUnits="userSpaceOnUse">
        <rect x="1" y="1" width="10" height="10" fill="#0000ff" stroke="#ffffff" stroke-width="2"/>
        <rect x="6" y="6" width="12" height="12" fill="#00ff00"/>
      </marker>)svg";
  };
  const auto looseMarker = [](const std::string& id) {
    return R"svg(<marker id=")svg" + id +
           R"svg(" markerWidth="8" markerHeight="8" refX="4" refY="4"
               markerUnits="userSpaceOnUse" overflow="visible">
        <rect width="8" height="8" fill="#ff00ff"/>
        <rect x="4" y="2" width="4" height="4" fill="#00ffff"/>
      </marker>)svg";
  };
  const std::string head =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">)svg";
  const std::string host =
      R"svg(<path d="M10 10 H20 V20 H30 V40 H10" fill="#ff0000" stroke="#000000" stroke-width="6")svg";
  const std::string yellow = R"svg(<rect x="24" y="30" width="20" height="16" fill="#ffff00"/>)svg";
  const std::string gray = R"svg(<rect x="44" y="50" width="6" height="14" fill="#808080"/>)svg";

  const std::string markers =
      head + clippedMarker("clipped") + looseMarker("loose") + host +
      R"svg( marker-start="url(#clipped)" marker-mid="url(#clipped)" marker-end="url(#clipped)"/>)svg" +
      yellow +
      R"svg(<path d="M40 56 H46 H52 H58" fill="none" marker-start="url(#loose)"
          marker-mid="url(#loose)" marker-end="url(#loose)"/>)svg" +
      gray + "</svg>";

  // One marker element and one zero-length path per placement, in the same order.
  std::string singles = head;
  std::string placements;
  int index = 0;
  for (const Vector2i vertex : {Vector2i(10, 10), Vector2i(20, 10), Vector2i(20, 20),
                                Vector2i(30, 20), Vector2i(30, 40), Vector2i(10, 40)}) {
    const std::string id = "c" + std::to_string(index++);
    singles += clippedMarker(id);
    const std::string at = std::to_string(vertex.x) + " " + std::to_string(vertex.y);
    placements += "<path d=\"M" + at + " L" + at + "\" marker-start=\"url(#" + id + ")\"/>";
  }
  placements += yellow;
  for (const int x : {40, 46, 52, 58}) {
    const std::string id = "l" + std::to_string(index++);
    singles += looseMarker(id);
    const std::string at = std::to_string(x) + " 56";
    placements += "<path d=\"M" + at + " L" + at + "\" marker-start=\"url(#" + id + ")\"/>";
  }
  singles += host + "/>" + placements + gray + "</svg>";

  SVGDocument referenceDocument = ParseDocumentOrFail(singles);
  RendererGeode reference = createRenderer();
  reference.draw(referenceDocument);
  const RendererBitmap expected = reference.takeSnapshot();
  ASSERT_THAT(expected.empty(), testing::IsFalse());
  // Premises: a later placement covers the one before it, and the marker viewport cuts the
  // spilling green square.
  ASSERT_THAT(PixelAt(expected, 43, 55), RgbaEq(255, 0, 255, 255));
  ASSERT_THAT(PixelAt(expected, 12, 42), RgbaEq(0, 0, 255, 255));
  ASSERT_THAT(PixelAt(expected, 21, 49), IsTransparent());

  SVGDocument document = ParseDocumentOrFail(markers);
  RendererGeode renderer = createRenderer();
  for (const char* frame : {"first", "steady"}) {
    SCOPED_TRACE(frame);
    renderer.draw(document);
    editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), expected,
                                         std::string("marker_placements_") + frame,
                                         editor::tests::PixelmatchIdentityParams());
  }
}

// A slot keeps a bounded number of repeat records. Repeats past the bound take a record for one
// frame instead: the picture is the same, and each such repeat writes its one 256-byte instance
// record every frame while the bounded ones write nothing.
TEST_F(RendererGeodeTest, MarkerRepeatsPastTheRecordLimitDrawTheSamePicture) {
  const std::string source = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
    <marker id="m" markerWidth="8" markerHeight="8" refX="4" refY="4">
      <rect width="8" height="8" fill="#3366cc"/>
    </marker>
    <polyline points="4,60 12,4 20,60 28,4 36,60 44,4 52,60" fill="none" stroke="#000000"
              marker-start="url(#m)" marker-mid="url(#m)" marker-end="url(#m)"/>
  </svg>)svg";
  constexpr uint64_t kRepeats = 6;
  constexpr uint64_t kKeptRecords = 2;

  SVGDocument unlimitedDocument = ParseDocumentOrFail(source);
  RendererGeode unlimited = createRenderer();
  unlimited.draw(unlimitedDocument);
  unlimited.draw(unlimitedDocument);
  ASSERT_THAT(unlimited.lastFrameTimings().counters.bufferWrites, testing::Eq(0u));
  const RendererBitmap expected = unlimited.takeSnapshot();
  ASSERT_THAT(expected.empty(), testing::IsFalse());

  SVGDocument document = ParseDocumentOrFail(source);
  RendererGeode limited = createRenderer();
  limited.setRepeatRecordLimitForTesting(kKeptRecords);
  limited.draw(document);
  limited.draw(document);
  const geode::GeodeCounters& counters = limited.lastFrameTimings().counters;
  EXPECT_THAT(counters.pathEncodes, testing::Eq(0u));
  EXPECT_THAT(counters.bufferWrites, testing::Eq(kRepeats - kKeptRecords));
  EXPECT_THAT(counters.bufferWriteBytes, testing::Eq((kRepeats - kKeptRecords) * 256u));
  editor::tests::CompareBitmapToBitmap(limited.takeSnapshot(), expected, "marker_record_limit",
                                       editor::tests::PixelmatchIdentityParams());
}

/// Pixels where \p actual differs from \p expected under the canonical identity comparison, as a
/// signal: every count passes, so a premise asserts on the returned count instead.
int DifferingPixels(const RendererBitmap& actual, const RendererBitmap& expected,
                    const std::string& label) {
  editor::tests::BitmapGoldenCompareParams signalParams = editor::tests::PixelmatchIdentityParams();
  signalParams.maxMismatchedPixels = std::numeric_limits<int>::max();
  int mismatchedPixels = -1;
  editor::tests::CompareBitmapToBitmap(actual, expected, label, signalParams, &mismatchedPixels);
  return mismatchedPixels;
}

/// One edit for \ref MarkerPlacementsFollowEveryEditThatMovesOrRepaintsThem: one or more DOM
/// steps, each followed by a render.
struct MarkerEdit {
  const char* label;                                     ///< Name in failure output and diffs.
  std::vector<std::function<void(SVGDocument&)>> steps;  ///< DOM steps, in order.
};

/// Sets \p attribute of the element with id \p id to \p value.
std::function<void(SVGDocument&)> SetAttributeEdit(std::string id, std::string attribute,
                                                   std::string value) {
  return [=](SVGDocument& document) {
    std::optional<SVGElement> element = document.querySelector("#" + id);
    ASSERT_THAT(element, testing::Optional(testing::_));
    element->setAttribute(xml::XMLQualifiedNameRef(attribute), value);
  };
}

/// Removes \p attribute from the element with id \p id.
std::function<void(SVGDocument&)> RemoveAttributeEdit(std::string id, std::string attribute) {
  return [=](SVGDocument& document) {
    std::optional<SVGElement> element = document.querySelector("#" + id);
    ASSERT_THAT(element, testing::Optional(testing::_));
    element->removeAttribute(xml::XMLQualifiedNameRef(attribute));
  };
}

/// Applies \p edit to the document's `#m` marker through its typed setters; the generic
/// `setAttribute` does not reach a marker's own attributes (#2621).
std::function<void(SVGDocument&)> MarkerElementEdit(std::function<void(SVGMarkerElement&)> edit) {
  return [=](SVGDocument& document) {
    std::optional<SVGElement> element = document.querySelector("#m");
    ASSERT_THAT(element, testing::Optional(testing::_));
    SVGMarkerElement marker = element->cast<SVGMarkerElement>();
    edit(marker);
  };
}

// Each edit changes what some marker placement draws. A placement after the first keeps its
// record across frames and rewrites it only when its bytes change, so an edit applied after two
// frames must render exactly what a fresh document carrying the same edit renders. A multi-step
// edit renders after every step and compares every step's frame with a fresh document carrying
// the steps so far.
//
// The edits run on two fixtures. With the default `orient`, every placement's marker viewport clip
// stays an axis-aligned rectangle, which each placement's record carries. With `orient="auto"` the
// placements turn and their viewport clip becomes a clip polygon, which the first placement's
// uniform and each later placement's kept batch uniform carry. Both are resident, so in both the
// steady frame must write nothing before the edit. `marker_orient_switches` turns the placements of
// either fixture, sets them axis-aligned, and turns them again.
TEST_F(RendererGeodeTest, MarkerPlacementsFollowEveryEditThatMovesOrRepaintsThem) {
  const std::string rotatedSource =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
    <linearGradient id="g"><stop offset="0" stop-color="#ff00ff"/>
      <stop offset="1" stop-color="#00ffff"/></linearGradient>
    <clipPath id="cp"><rect width="1" height="6"/></clipPath>
    <marker id="m" viewBox="0 0 6 6" markerWidth="6" markerHeight="4" refX="3" refY="3"
            orient="auto">
      <path id="content" d="M0 0 L6 3 L0 6 Z" fill="context-stroke" stroke="context-fill"
            stroke-width="0.75"/>
      <rect id="dot" width="2" height="2" fill="currentColor" color="#00aa00"/>
    </marker>
    <g id="hostgroup">
      <path id="host" d="M6 52 L18 10 L30 52 L42 10 L58 40" fill="#ffcc00" stroke="#cc0000"
            stroke-width="1.5" marker-start="url(#m)" marker-mid="url(#m)" marker-end="url(#m)"/>
    </g>
  </svg>)svg";
  std::string axisAlignedSource = rotatedSource;
  const std::string orientAuto = "\n            orient=\"auto\"";
  ASSERT_THAT(axisAlignedSource.find(orientAuto), testing::Ne(std::string::npos));
  axisAlignedSource.erase(axisAlignedSource.find(orientAuto), orientAuto.size());

  const MarkerEdit edits[] = {
      {"path_vertices", {SetAttributeEdit("host", "d", "M6 52 L18 14 L30 48 L42 14 L58 40")}},
      {"path_vertex_count", {SetAttributeEdit("host", "d", "M6 52 L18 10 L58 40")}},
      {"host_transform", {SetAttributeEdit("host", "transform", "rotate(5 32 32)")}},
      {"ancestor_transform", {SetAttributeEdit("hostgroup", "transform", "translate(2 3)")}},
      {"stroke_width_scales_markers", {SetAttributeEdit("host", "stroke-width", "2.5")}},
      {"marker_ref_x", {MarkerElementEdit([](SVGMarkerElement& marker) {
         marker.setRefX(Lengthd(1, Lengthd::Unit::None));
       })}},
      {"marker_orient", {MarkerElementEdit([](SVGMarkerElement& marker) {
         marker.setOrient(MarkerOrient::AngleDegrees(30));
       })}},
      {"marker_orient_switches",
       {MarkerElementEdit(
            [](SVGMarkerElement& marker) { marker.setOrient(MarkerOrient::AngleDegrees(30)); }),
        MarkerElementEdit(
            [](SVGMarkerElement& marker) { marker.setOrient(MarkerOrient::AngleDegrees(0)); }),
        MarkerElementEdit(
            [](SVGMarkerElement& marker) { marker.setOrient(MarkerOrient::Auto()); })}},
      {"marker_ref_y", {MarkerElementEdit([](SVGMarkerElement& marker) {
         marker.setRefY(Lengthd(1, Lengthd::Unit::None));
       })}},
      {"marker_width", {MarkerElementEdit([](SVGMarkerElement& marker) {
         marker.setMarkerWidth(Lengthd(3, Lengthd::Unit::None));
       })}},
      {"marker_height", {MarkerElementEdit([](SVGMarkerElement& marker) {
         marker.setMarkerHeight(Lengthd(6, Lengthd::Unit::None));
       })}},
      {"marker_view_box", {MarkerElementEdit([](SVGMarkerElement& marker) {
         marker.setViewBox(Box2d::FromXYWH(0, 0, 8, 8));
       })}},
      {"marker_aspect_ratio", {MarkerElementEdit([](SVGMarkerElement& marker) {
         marker.setPreserveAspectRatio(PreserveAspectRatio::None());
       })}},
      {"marker_units", {MarkerElementEdit([](SVGMarkerElement& marker) {
         marker.setMarkerUnits(MarkerUnits::UserSpaceOnUse);
       })}},
      {"marker_overflow", {SetAttributeEdit("m", "overflow", "visible")}},
      {"content_geometry", {SetAttributeEdit("content", "d", "M0 0 L6 2 L0 6 Z")}},
      {"content_stroke_width", {SetAttributeEdit("content", "stroke-width", "1.5")}},
      {"content_fill_color", {SetAttributeEdit("dot", "fill", "#123456")}},
      {"content_fill_opacity", {SetAttributeEdit("dot", "fill-opacity", "0.5")}},
      {"current_color", {SetAttributeEdit("dot", "color", "#aa00aa")}},
      {"content_visibility", {SetAttributeEdit("dot", "visibility", "hidden")}},
      {"content_display", {SetAttributeEdit("dot", "display", "none")}},
      {"content_clip_path_added_then_removed",
       {SetAttributeEdit("dot", "clip-path", "url(#cp)"), RemoveAttributeEdit("dot", "clip-path")}},
      {"content_solid_gradient_solid",
       {SetAttributeEdit("dot", "fill", "url(#g)"), SetAttributeEdit("dot", "fill", "#0000aa")}},
      // Removing a marker property drops placements and shifts the repeat each later placement
      // takes. Switching it to another marker after a render is a separate defect (#2620).
      {"marker_property", {RemoveAttributeEdit("host", "marker-mid")}},
      {"context_stroke", {SetAttributeEdit("host", "stroke", "#0066ff")}},
      {"context_fill", {SetAttributeEdit("host", "fill", "#00ccff")}},
  };

  for (const auto& [fixture, source] :
       {std::pair<std::string, std::string>{"axis_aligned", axisAlignedSource},
        std::pair<std::string, std::string>{"rotated", rotatedSource}}) {
    for (const MarkerEdit& edit : edits) {
      SCOPED_TRACE(fixture + ": " + edit.label);
      SVGDocument document = ParseDocumentOrFail(source);
      RendererGeode renderer = createRenderer();
      renderer.draw(document);
      renderer.draw(document);
      // Premise: the steady frame rewrites nothing, so the placements are drawing from records and
      // uniforms kept across frames and the edit is what has to reach them.
      ASSERT_THAT(renderer.lastFrameTimings().counters.bufferWrites, testing::Eq(0u));
      const RendererBitmap before = renderer.takeSnapshot();

      ASSERT_THAT(before.empty(), testing::IsFalse());
      for (size_t stepCount = 1; stepCount <= edit.steps.size(); ++stepCount) {
        SCOPED_TRACE(stepCount);
        ASSERT_NO_FATAL_FAILURE(edit.steps[stepCount - 1](document));
        renderer.draw(document);
        const RendererBitmap actual = renderer.takeSnapshot();

        // A document of its own per step: a document drawn before carries residence of its own.
        SVGDocument freshDocument = ParseDocumentOrFail(source);
        for (size_t i = 0; i < stepCount; ++i) {
          ASSERT_NO_FATAL_FAILURE(edit.steps[i](freshDocument));
        }
        RendererGeode freshRenderer = createRenderer();
        freshRenderer.draw(freshDocument);
        const RendererBitmap expected = freshRenderer.takeSnapshot();
        ASSERT_THAT(expected.empty(), testing::IsFalse());

        // Premise: the first step changes the picture, so matching the fresh render is not vacuous.
        if (stepCount == 1) {
          const int changed = DifferingPixels(
              actual, before, std::string("marker_edit_premise_") + fixture + "_" + edit.label);
          EXPECT_THAT(changed, testing::Gt(0)) << "the first step does not change the picture";
          if (changed <= 0) {
            break;
          }
        }
        editor::tests::CompareBitmapToBitmap(actual, expected,
                                             std::string("marker_edit_") + fixture + "_" +
                                                 edit.label + "_step_" + std::to_string(stepCount),
                                             editor::tests::PixelmatchIdentityParams());
      }
    }
  }
}

// Placements of an `orient="auto"` marker turn with their path, and so does each placement's
// marker viewport clip, which then reaches the renderer as a clip polygon. Here the content spills
// out of the viewport on both sides and is cut by it. Six placements of one marker, on three paths
// of different directions, must match the same placements drawn by six markers placed once each,
// and the steady frame, which draws every placement from residence and so writes and binds
// nothing, must match a fresh render.
TEST_F(RendererGeodeTest, RotatedClippedMarkerPlacementsMatchSinglePlacementMarkers) {
  const auto marker = [](const std::string& id, const std::string& overflow) {
    return R"svg(<marker id=")svg" + id + R"svg(" markerWidth="8" markerHeight="8" refX="4" refY="4"
        orient="auto" markerUnits="userSpaceOnUse" overflow=")svg" +
           overflow + R"svg(">
        <rect x="-3" y="2" width="14" height="4" fill="#0044cc"/>
        <rect x="2" y="-3" width="4" height="14" fill="#cc4400" fill-opacity="0.75"/>
      </marker>)svg";
  };
  const std::vector<std::string> paths = {"M8 50 L28 30", "M30 56 L54 44", "M40 30 L50 8"};
  const auto document = [&](bool shared, const std::string& overflow) {
    std::string source = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">)svg";
    for (std::size_t i = 0; i < paths.size(); ++i) {
      source += marker(shared ? "m" : "s" + std::to_string(i), overflow);
      source += marker(shared ? "m" : "e" + std::to_string(i), overflow);
    }
    for (std::size_t i = 0; i < paths.size(); ++i) {
      const std::string start = shared ? "m" : "s" + std::to_string(i);
      const std::string end = shared ? "m" : "e" + std::to_string(i);
      source += "<path d=\"" + paths[i] + "\" stroke=\"#000000\" stroke-width=\"2\" " +
                "marker-start=\"url(#" + start + ")\" marker-end=\"url(#" + end + ")\"/>";
    }
    return source + "</svg>";
  };
  const auto render = [this](const std::string& source) {
    SVGDocument parsed = ParseDocumentOrFail(source);
    RendererGeode renderer = createRenderer();
    renderer.draw(parsed);
    return renderer.takeSnapshot();
  };
  const RendererBitmap singles = render(document(false, "hidden"));
  ASSERT_THAT(
      DifferingPixels(render(document(false, "visible")), singles, "rotated_marker_clip_premise"),
      testing::Gt(0))
      << "the marker viewport must cut the content, or the clip is not exercised";

  const std::string shared = document(true, "hidden");
  const RendererBitmap fresh = render(shared);
  editor::tests::CompareBitmapToBitmap(fresh, singles, "rotated_marker_placements_first",
                                       editor::tests::PixelmatchIdentityParams());
  SVGDocument parsed = ParseDocumentOrFail(shared);
  RendererGeode renderer = createRenderer();
  renderer.draw(parsed);
  renderer.draw(parsed);
  const geode::GeodeCounters& steady = renderer.lastFrameTimings().counters;
  EXPECT_THAT(steady.bufferWrites, testing::Eq(0u))
      << "turned placements must keep their geometry, records and clip polygons resident";
  EXPECT_THAT(steady.bindgroupCreates, testing::Eq(0u))
      << "turned placements must keep their bind groups";
  editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), fresh,
                                       "rotated_marker_placements_steady",
                                       editor::tests::PixelmatchIdentityParams());
}

// A render snapshot of draws under a clip polygon, turned marker placements and a turned nested
// viewport, holds no handle into the live document, so a replay of it reaches no residence there
// and draws what a direct draw draws.
TEST_F(RendererGeodeTest, ClipPolygonSnapshotsHoldNoLiveReference) {
  const std::string source = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
    <marker id="m" markerWidth="8" markerHeight="8" refX="4" refY="4" orient="auto">
      <path d="M-2 -2 L10 4 L-2 10 Z" fill="#c00" stroke="#003"/>
    </marker>
    <polyline points="4,60 16,8 28,56 40,8 60,40" fill="none" stroke="#333"
              marker-start="url(#m)" marker-mid="url(#m)" marker-end="url(#m)"/>
    <g transform="rotate(20 32 32)">
      <svg x="20" y="20" width="24" height="24"><rect x="-4" y="-4" width="40" height="40"
        fill="#2050c0"/></svg>
    </g>
  </svg>)svg";
  SVGDocument document = ParseDocumentOrFail(source);
  RendererGeode renderer = createRenderer();
  RendererDriver driver(renderer);
  const RenderSnapshot snapshot = driver.captureRenderSnapshot(document);
  EXPECT_THAT(snapshot.liveRegistryReferenceCountForTesting(document.registry()), testing::Eq(0u))
      << "a recorded draw under a clip polygon must not keep a handle into the live document";

  driver.draw(snapshot);
  const RendererBitmap replayed = renderer.takeSnapshot();
  ASSERT_THAT(test::CountNonTransparentPixels(replayed), testing::Gt(0u));
  editor::tests::CompareBitmapToBitmap(replayed, RenderFreshGeode(sharedDevice(), source),
                                       "clip_polygon_snapshot_replay",
                                       editor::tests::PixelmatchIdentityParams());
}

// A frame drawn at a new surface transform, as a compositor draws a layer it moves, keeps the
// document's residence: no edit rebuilds the render tree, so every resident draw keeps its slot and
// every turned marker placement after the first keeps its occurrence, and only the polygons
// change. Each uniform carrying a polygon must be rewritten, so the warm frame must render exactly
// what a fresh renderer renders at the new transform.
TEST_F(RendererGeodeTest, ClipPolygonUniformsFollowTheSurfaceTransform) {
  const std::string source = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
    <marker id="m" markerWidth="8" markerHeight="8" refX="4" refY="4" orient="auto"
            markerUnits="userSpaceOnUse">
      <rect x="-3" y="2" width="14" height="4" fill="#0044cc"/>
      <rect x="2" y="-3" width="4" height="14" fill="#cc4400" fill-opacity="0.75"/>
    </marker>
    <path d="M8 50 L28 30 L50 40 L40 10" fill="none" stroke="#000000" stroke-width="2"
          marker-start="url(#m)" marker-mid="url(#m)" marker-end="url(#m)"/>
    <g transform="rotate(20 32 32)">
      <svg x="10" y="10" width="20" height="20"><rect x="-4" y="-4" width="30" height="30"
        fill="#208040"/></svg>
    </g>
  </svg>)svg";
  RenderViewport viewport;
  viewport.size = Vector2d(64.0, 64.0);
  const Transform2d surfaceFromCanvas = Transform2d::Rotate(MathConstants<double>::kPi / 12.0) *
                                        Transform2d::Translate(Vector2d(6.0, -4.0));

  SVGDocument document = ParseDocumentOrFail(source);
  RendererGeode warm = createRenderer();
  RendererDriver warmDriver(warm);
  warmDriver.draw(document, viewport, Transform2d());
  warmDriver.draw(document, viewport, Transform2d());
  // Premise: the steady frame rewrites nothing, so the draws read uniforms kept across frames.
  ASSERT_THAT(warm.lastFrameTimings().counters.bufferWrites, testing::Eq(0u));
  const RendererBitmap before = warm.takeSnapshot();
  warmDriver.draw(document, viewport, surfaceFromCanvas);
  const RendererBitmap actual = warm.takeSnapshot();

  SVGDocument freshDocument = ParseDocumentOrFail(source);
  RendererGeode fresh = createRenderer();
  RendererDriver freshDriver(fresh);
  freshDriver.draw(freshDocument, viewport, surfaceFromCanvas);
  const RendererBitmap expected = fresh.takeSnapshot();
  ASSERT_THAT(DifferingPixels(expected, before, "clip_polygon_surface_transform_premise"),
              testing::Gt(0))
      << "the new transform must change the picture";
  editor::tests::CompareBitmapToBitmap(actual, expected, "clip_polygon_surface_transform",
                                       editor::tests::PixelmatchIdentityParams());
}

/// The scene of \ref ClipPolygonDrawsFollowEveryEditOfThePolygon and
/// \ref ClipPolygonResidentDrawsMatchTheArenaDraws: a nested viewport turned with its group over
/// a solid fill, a pattern fill, a linear gradient fill with a solid stroke, a radial gradient fill
/// and the four placements of a turned marker, whose content its marker viewport cuts. Every shape
/// spills past the nested viewport.
/// @param viewportOverflow The nested viewport's `overflow`: `hidden` clips the shapes by the
///   polygon, `visible` does not.
std::string ClipPolygonSceneSvg(std::string_view viewportOverflow = "hidden") {
  return R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
    <linearGradient id="g" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#2050c0"/>
      <stop offset="1" stop-color="#c05020"/></linearGradient>
    <radialGradient id="r"><stop offset="0" stop-color="#f0e040"/>
      <stop offset="1" stop-color="#4020a0"/></radialGradient>
    <pattern id="p" patternUnits="userSpaceOnUse" width="4" height="4">
      <rect width="2" height="2" fill="#208040"/></pattern>
    <marker id="m" markerWidth="6" markerHeight="6" refX="3" refY="3" orient="auto"
            markerUnits="userSpaceOnUse">
      <rect x="-3" y="1" width="12" height="4" fill="#7030a0"/>
    </marker>
    <g id="turn" transform="rotate(20 32 32)">
      <svg x="8" y="12" width="48" height="40" viewBox="0 0 48 40" overflow=")svg" +
         std::string(viewportOverflow) + R"svg(">
        <rect x="-6" y="-6" width="30" height="26" fill="#e0c020"/>
        <rect x="24" y="-6" width="30" height="26" fill="url(#p)"/>
        <circle cx="12" cy="30" r="14" fill="url(#g)" stroke="#102030" stroke-width="2"/>
        <ellipse cx="36" cy="30" rx="16" ry="12" fill="url(#r)"/>
        <polyline points="4,36 16,30 28,36 40,30" fill="none" stroke="none"
                  marker-start="url(#m)" marker-mid="url(#m)" marker-end="url(#m)"/>
      </svg>
    </g>
  </svg>)svg";
}

// Draws under a clip polygon that cuts them paint exactly what the per-frame arena paints for the
// same draws, which a renderer whose document budget keeps no resident bytes draws: a solid, a
// pattern, a linear and a radial gradient fill drawn once each from their slots' uniforms, and the
// turned marker placements after the first drawn as repeats from batches binding their occurrences'
// clip uniforms.
TEST_F(RendererGeodeTest, ClipPolygonResidentDrawsMatchTheArenaDraws) {
  const auto steadyFrame = [&](const std::string& source, std::uint64_t residentBytes) {
    SVGDocument document = ParseDocumentOrFail(source);
    RendererGeode renderer = createRenderer();
    renderer.setGeometryBudgetForTesting(/*maximumDraws=*/4096u, /*maximumItems=*/1u << 20,
                                         /*maximumFrameBytes=*/64u << 20,
                                         /*maximumCacheBytes=*/64u << 20,
                                         /*maximumResidentBytes=*/residentBytes);
    renderer.draw(document);
    renderer.draw(document);
    const geode::GeodeCounters counters = renderer.lastFrameTimings().counters;
    return std::make_tuple(counters.bufferWrites, counters.bindgroupCreates,
                           renderer.takeSnapshot());
  };
  const auto [residentWrites, residentBindGroups, resident] =
      steadyFrame(ClipPolygonSceneSvg(), 64u << 20);
  const auto [arenaWrites, arenaBindGroups, arena] = steadyFrame(ClipPolygonSceneSvg(), 0u);
  const RendererBitmap unclipped =
      std::get<2>(steadyFrame(ClipPolygonSceneSvg("visible"), 64u << 20));
  ASSERT_THAT(residentWrites, testing::Eq(0u))
      << "Premise: the draws under the polygon are resident";
  ASSERT_THAT(residentBindGroups, testing::Eq(0u))
      << "Premise: the draws under the polygon are resident";
  ASSERT_THAT(arenaBindGroups, testing::Gt(0u))
      << "Premise: with no resident bytes the draws build bind groups of their own every frame, so "
         "they draw from the arena";
  ASSERT_THAT(DifferingPixels(resident, unclipped, "clip_polygon_cut_premise"), testing::Gt(0))
      << "Premise: the polygon must cut the shapes, or a wrong polygon would pass unnoticed";

  editor::tests::CompareBitmapToBitmap(resident, arena, "clip_polygon_resident_vs_arena",
                                       editor::tests::PixelmatchIdentityParams());
}

// A nested viewport turned with its group reaches the renderer as a clip polygon over one draw of
// each resident class, every one of which spills past the viewport, and over the placements of a
// turned marker, whose content its own marker viewport cuts and whose placements after the first
// are same-frame repeats. A warm renderer keeps each draw's uniform, and each repeat's batch
// uniform, polygon included, across frames and rewrites it only when its bytes change, so each
// edit that moves, resizes, turns, skews, straightens or zooms the polygons must render exactly
// what a fresh document carrying the same edits renders. A multi-step edit renders after every
// step. Edits of the clip rectangle itself are the marker viewport edits of
// `MarkerPlacementsFollowEveryEditThatMovesOrRepaintsThem`.
TEST_F(RendererGeodeTest, ClipPolygonDrawsFollowEveryEditOfThePolygon) {
  const std::string source = ClipPolygonSceneSvg();

  const MarkerEdit edits[] = {
      {"turn_further", {SetAttributeEdit("turn", "transform", "rotate(35 32 32)")}},
      {"skew", {SetAttributeEdit("turn", "transform", "rotate(20 32 32) skewX(10)")}},
      {"move", {SetAttributeEdit("turn", "transform", "translate(5 -3) rotate(20 32 32)")}},
      {"resize", {SetAttributeEdit("turn", "transform", "rotate(20 32 32) scale(0.8)")}},
      {"straighten_then_turn",
       {SetAttributeEdit("turn", "transform", "rotate(0 32 32)"),
        SetAttributeEdit("turn", "transform", "rotate(20 32 32)")}},
      // A new canvas size scales every polygon in device pixels without editing the document.
      {"zoom", {[](SVGDocument& document) { document.setCanvasSize(96, 96); }}},
  };

  for (const MarkerEdit& edit : edits) {
    SCOPED_TRACE(edit.label);
    SVGDocument document = ParseDocumentOrFail(source);
    RendererGeode renderer = createRenderer();
    renderer.draw(document);
    renderer.draw(document);
    // Premise: the steady frame rewrites nothing, so each draw reads a uniform kept across frames
    // and the edit is what has to reach it.
    ASSERT_THAT(renderer.lastFrameTimings().counters.bufferWrites, testing::Eq(0u));
    const RendererBitmap before = renderer.takeSnapshot();
    ASSERT_THAT(before.pixels, testing::Not(testing::IsEmpty()));

    for (size_t stepCount = 1; stepCount <= edit.steps.size(); ++stepCount) {
      SCOPED_TRACE(stepCount);
      ASSERT_NO_FATAL_FAILURE(edit.steps[stepCount - 1](document));
      renderer.draw(document);
      const RendererBitmap actual = renderer.takeSnapshot();

      SVGDocument freshDocument = ParseDocumentOrFail(source);
      for (size_t i = 0; i < stepCount; ++i) {
        ASSERT_NO_FATAL_FAILURE(edit.steps[i](freshDocument));
      }
      RendererGeode freshRenderer = createRenderer();
      freshRenderer.draw(freshDocument);
      const RendererBitmap expected = freshRenderer.takeSnapshot();
      ASSERT_THAT(expected.pixels, testing::Not(testing::IsEmpty()));

      // Premise: the first step changes the picture, so matching the fresh render is not vacuous.
      // A zoom changes the canvas size by construction.
      if (stepCount == 1 && actual.dimensions == before.dimensions) {
        EXPECT_THAT(
            DifferingPixels(actual, before, std::string("clip_polygon_edit_premise_") + edit.label),
            testing::Gt(0))
            << "the first step does not change the picture";
      }
      editor::tests::CompareBitmapToBitmap(
          actual, expected,
          std::string("clip_polygon_edit_") + edit.label + "_step_" + std::to_string(stepCount),
          editor::tests::PixelmatchIdentityParams());
    }
  }
}

// An offscreen pass that draws the same document inside an open outer frame must not rewrite the
// repeat records the outer frame's recorded draws read when it submits: every buffer write in a
// frame executes before every draw. The document changes between the outer draw and the inner
// one, so a rewrite would move the outer frame's placements to the inner frame's positions.
TEST_F(RendererGeodeTest, OffscreenPassLeavesTheOuterFramesRepeatRecordsAlone) {
  const std::string head = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
    <marker id="m" markerWidth="8" markerHeight="8" refX="4" refY="4">
      <rect width="8" height="8" fill="#3366cc"/>
    </marker>)svg";
  const std::string original = head + R"svg(<path id="host" d="M6 32 H18 H30 H42 H54" fill="none"
      marker-start="url(#m)" marker-mid="url(#m)" marker-end="url(#m)"/></svg>)svg";
  const std::string edited = head + R"svg(<path id="host" d="M6 12 H18 H30 H42 H54" fill="none"
      marker-start="url(#m)" marker-mid="url(#m)" marker-end="url(#m)"/></svg>)svg";
  const auto render = [this](const std::string& source) {
    SVGDocument document = ParseDocumentOrFail(source);
    RendererGeode renderer = createRenderer();
    renderer.draw(document);
    return renderer.takeSnapshot();
  };
  const RendererBitmap expectedOuter = render(original);
  const RendererBitmap expectedInner = render(edited);
  ASSERT_THAT(DifferingPixels(expectedInner, expectedOuter, "offscreen_repeat_premise"),
              testing::Gt(0))
      << "the edit must move the placements, or the outer frame cannot show a rewrite";

  SVGDocument document = ParseDocumentOrFail(original);
  RendererGeode outer = createRenderer();
  outer.draw(document);  // The repeat records exist and hold the original placements.
  RenderViewport viewport;
  viewport.size = Vector2d(64.0, 64.0);
  outer.beginFrame(viewport);
  RendererDriver(outer).drawDocumentIntoCurrentFrame(document, viewport, Transform2d());

  std::optional<SVGElement> host = document.querySelector("#host");
  ASSERT_THAT(host, testing::Optional(testing::_));
  host->setAttribute("d", "M6 12 H18 H30 H42 H54");
  std::unique_ptr<RendererInterface> inner = outer.createOffscreenInstance();
  ASSERT_THAT(inner, testing::NotNull());
  inner->draw(document);
  const RendererBitmap innerBitmap = inner->takeSnapshot();
  outer.endFrame();

  editor::tests::CompareBitmapToBitmap(outer.takeSnapshot(), expectedOuter,
                                       "offscreen_repeat_outer_frame",
                                       editor::tests::PixelmatchIdentityParams());
  editor::tests::CompareBitmapToBitmap(innerBitmap, expectedInner, "offscreen_repeat_inner_pass",
                                       editor::tests::PixelmatchIdentityParams());
}

// One source path stroked with several styles keeps one resident slot per stroke variant, and a
// repeat draw of one variant takes a record of that variant's slot. Here the outer frame draws
// three repeats of variant A and then, inside the still-open frame, an offscreen pass on the same
// device draws a document state whose new style finds the stroke cache full and replaces another
// variant of the same path. Variant A is not a candidate, since the open outer frame may still
// read it; the replaced variant's slot is reset while the outer frame's draws of A are recorded
// and not yet submitted. Both images must match fresh renders of their states exactly.
TEST_F(RendererGeodeTest, OffscreenPassReplacingAStrokeVariantLeavesTheOuterFramesRepeatsAlone) {
  const auto document_source = [](int repeatX, int replacedWidth, bool othersShown) {
    std::string source = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
      <defs><path id="p" d="M4 6 C20 0 36 14 56 6" fill="none"/></defs>)svg";
    for (int i = 0; i < 3; ++i) {
      source += "<use id=\"a" + std::to_string(i) + "\" href=\"#p\" x=\"" +
                std::to_string(repeatX + i * 4) + "\" y=\"" + std::to_string(i * 6) +
                "\" stroke=\"#cc2200\" stroke-width=\"2\"/>";
    }
    // One instance per other cached style, so the source path's stroke cache is full.
    for (std::size_t i = 1; othersShown && i < geode::kGeodeStrokeVariantCount; ++i) {
      const std::size_t width = i == 1 ? replacedWidth : 2 + i;
      source += "<use id=\"o" + std::to_string(i) + "\" href=\"#p\" y=\"" +
                std::to_string(20 + 4 * i) + "\" stroke=\"#2255aa\" stroke-width=\"" +
                std::to_string(width) + "\"/>";
    }
    return source + "</svg>";
  };
  const std::string repeatsOnly = document_source(0, 3, false);
  const std::string replacedStyle = document_source(6, 11, true);

  const auto render = [this](const std::string& source) {
    SVGDocument document = ParseDocumentOrFail(source);
    RendererGeode renderer = createRenderer();
    renderer.draw(document);
    return renderer.takeSnapshot();
  };
  const RendererBitmap expectedOuter = render(repeatsOnly);
  const RendererBitmap expectedInner = render(replacedStyle);
  ASSERT_THAT(DifferingPixels(expectedInner, expectedOuter, "stroke_variant_offscreen_premise"),
              testing::Gt(0))
      << "the two states must differ, or the outer frame cannot show the inner pass's draws";

  SVGDocument document = ParseDocumentOrFail(document_source(0, 3, true));
  const auto element = [&document](const std::string& id) {
    std::optional<SVGElement> found = document.querySelector("#" + id);
    EXPECT_THAT(found, testing::Optional(testing::_)) << id;
    return found;
  };
  RendererGeode outer = createRenderer();
  outer.draw(document);  // Fills every stroke variant of the source path.
  for (std::size_t i = 1; i < geode::kGeodeStrokeVariantCount; ++i) {
    element("o" + std::to_string(i))->setAttribute("display", "none");
  }
  outer.draw(document);  // Variant A's repeat records now hold the repeats-only placements.
  RenderViewport viewport;
  viewport.size = Vector2d(64.0, 64.0);
  outer.beginFrame(viewport);
  RendererDriver(outer).drawDocumentIntoCurrentFrame(document, viewport, Transform2d());

  // The inner pass's state: the other styles are drawn again, one of them with a new width that
  // replaces a cached variant, and variant A's repeats move.
  for (std::size_t i = 1; i < geode::kGeodeStrokeVariantCount; ++i) {
    element("o" + std::to_string(i))->removeAttribute("display");
  }
  element("o1")->setAttribute("stroke-width", "11");
  for (int i = 0; i < 3; ++i) {
    element("a" + std::to_string(i))->setAttribute("x", std::to_string(6 + i * 4));
  }
  std::unique_ptr<RendererInterface> inner = outer.createOffscreenInstance();
  ASSERT_THAT(inner, testing::NotNull());
  inner->draw(document);
  const RendererBitmap innerBitmap = inner->takeSnapshot();
  outer.endFrame();

  editor::tests::CompareBitmapToBitmap(outer.takeSnapshot(), expectedOuter,
                                       "stroke_variant_offscreen_outer_frame",
                                       editor::tests::PixelmatchIdentityParams());
  editor::tests::CompareBitmapToBitmap(innerBitmap, expectedInner,
                                       "stroke_variant_offscreen_inner_pass",
                                       editor::tests::PixelmatchIdentityParams());

  // Premise: the inner pass replaced a cached variant rather than drawing the new style from a
  // per-frame encode, so the next frame of the same state encodes nothing.
  outer.draw(document);
  EXPECT_THAT(outer.lastFrameTimings().counters.pathEncodes, testing::Eq(0u))
      << "the new style must have taken a cached stroke variant in the inner pass";
  editor::tests::CompareBitmapToBitmap(outer.takeSnapshot(), expectedInner,
                                       "stroke_variant_offscreen_next_frame",
                                       editor::tests::PixelmatchIdentityParams());
}

// A path's fill slot serves both its solid and its pattern fills. Here `<use>` instances of one
// path alternate pattern and solid paint, so in each frame a solid repeat follows a draw that bound
// a pattern tile to the slot, and a pattern draw follows a solid one. A solid repeat draws through
// a scene batch, which binds no pattern tile, and a pattern-painted draw never takes a repeat
// record. Every edge sits on a pixel boundary, so the instances must match the same paint on
// separate rectangles exactly, on the first and the steady frame.
TEST_F(RendererGeodeTest, UseRepeatsOfAPatternPaintedSlotMatchPlainGeometry) {
  const std::string pattern = R"svg(
    <pattern id="pt" width="8" height="8" patternUnits="userSpaceOnUse">
      <rect width="4" height="4" fill="#cc2200"/><rect x="4" y="4" width="4" height="4"
      fill="#2255aa"/>
    </pattern>)svg";
  const std::string source =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">)svg" + pattern +
      R"svg(<defs><rect id="r" width="16" height="16"/></defs>
    <use href="#r" x="8" y="8" fill="url(#pt)"/>
    <use href="#r" x="24" y="8" fill="#00aa00"/>
    <use href="#r" x="40" y="8" fill="url(#pt)"/>
    <use href="#r" x="8" y="32" fill="#00aa00"/>
    <use href="#r" x="24" y="32" fill="#aa00aa"/>
    <use href="#r" x="40" y="32" fill="url(#pt)"/>
  </svg>)svg";
  const std::string plain =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">)svg" + pattern +
      R"svg(
    <rect x="8" y="8" width="16" height="16" fill="url(#pt)"/>
    <rect x="24" y="8" width="16" height="16" fill="#00aa00"/>
    <rect x="40" y="8" width="16" height="16" fill="url(#pt)"/>
    <rect x="8" y="32" width="16" height="16" fill="#00aa00"/>
    <rect x="24" y="32" width="16" height="16" fill="#aa00aa"/>
    <rect x="40" y="32" width="16" height="16" fill="url(#pt)"/>
  </svg>)svg";

  SVGDocument plainDocument = ParseDocumentOrFail(plain);
  RendererGeode reference = createRenderer();
  reference.draw(plainDocument);
  const RendererBitmap expected = reference.takeSnapshot();

  SVGDocument document = ParseDocumentOrFail(source);
  RendererGeode renderer = createRenderer();
  for (const char* frame : {"first", "steady"}) {
    SCOPED_TRACE(frame);
    renderer.draw(document);
    editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), expected,
                                         std::string("pattern_slot_repeats_") + frame,
                                         editor::tests::PixelmatchIdentityParams());
  }
}

// Repeated draws of a `<use>` source take the same repeat records as marker placements: adjacent
// and non-adjacent repeats, with differing inherited paint, interleaved with an overlapping
// element. Every edge sits on a pixel boundary, so the instances must match plain rectangles
// exactly, on the first and the steady frame, and an edit to the source or to one instance must
// match a fresh document carrying the same edit.
TEST_F(RendererGeodeTest, UseRepeatsMatchPlainGeometryAndFollowEdits) {
  const std::string source = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
    <defs><rect id="r" width="12" height="12"/></defs>
    <use id="u1" href="#r" x="4" y="4" fill="#ff0000"/>
    <rect x="10" y="10" width="12" height="12" fill="#0000ff"/>
    <use href="#r" x="16" y="16" fill="#00ff00"/>
    <use href="#r" x="22" y="22" fill="#00ff00"/>
    <use id="u5" href="#r" x="28" y="28" fill="#ffff00"/>
    <use href="#r" x="40" y="8" fill="#ff00ff"/>
  </svg>)svg";
  const std::string plain = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
    <rect x="4" y="4" width="12" height="12" fill="#ff0000"/>
    <rect x="10" y="10" width="12" height="12" fill="#0000ff"/>
    <rect x="16" y="16" width="12" height="12" fill="#00ff00"/>
    <rect x="22" y="22" width="12" height="12" fill="#00ff00"/>
    <rect x="28" y="28" width="12" height="12" fill="#ffff00"/>
    <rect x="40" y="8" width="12" height="12" fill="#ff00ff"/>
  </svg>)svg";

  SVGDocument plainDocument = ParseDocumentOrFail(plain);
  RendererGeode reference = createRenderer();
  reference.draw(plainDocument);
  const RendererBitmap expected = reference.takeSnapshot();

  SVGDocument document = ParseDocumentOrFail(source);
  RendererGeode renderer = createRenderer();
  for (const char* frame : {"first", "steady"}) {
    SCOPED_TRACE(frame);
    renderer.draw(document);
    editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), expected,
                                         std::string("use_repeats_") + frame,
                                         editor::tests::PixelmatchIdentityParams());
  }
  // Premise: the steady frame draws every repeat from records kept across frames.
  EXPECT_THAT(renderer.lastFrameTimings().counters.bufferWrites, testing::Eq(0u));
  EXPECT_THAT(renderer.lastFrameTimings().counters.bindgroupCreates, testing::Eq(0u));

  for (const auto& [label, edit] :
       std::vector<std::pair<std::string, std::function<void(SVGDocument&)>>>{
           {"source_width", SetAttributeEdit("r", "width", "6")},
           {"first_instance_position", SetAttributeEdit("u1", "x", "2")},
           {"repeat_instance_position", SetAttributeEdit("u5", "x", "30")},
           {"repeat_instance_paint", SetAttributeEdit("u5", "fill", "#00ffff")}}) {
    SCOPED_TRACE(label);
    SVGDocument edited = ParseDocumentOrFail(source);
    RendererGeode editedRenderer = createRenderer();
    editedRenderer.draw(edited);
    editedRenderer.draw(edited);
    ASSERT_NO_FATAL_FAILURE(edit(edited));
    editedRenderer.draw(edited);

    SVGDocument fresh = ParseDocumentOrFail(source);
    ASSERT_NO_FATAL_FAILURE(edit(fresh));
    RendererGeode freshRenderer = createRenderer();
    freshRenderer.draw(fresh);
    const RendererBitmap freshBitmap = freshRenderer.takeSnapshot();
    ASSERT_THAT(DifferingPixels(freshBitmap, expected, "use_repeats_premise_" + label),
                testing::Gt(0))
        << "the edit must change the picture, or matching the fresh render is vacuous";
    editor::tests::CompareBitmapToBitmap(editedRenderer.takeSnapshot(), freshBitmap,
                                         "use_repeats_edit_" + label,
                                         editor::tests::PixelmatchIdentityParams());
  }
}

}  // namespace
}  // namespace donner::svg

namespace donner::gpu {

/// Prints a resource identity as `(slot, generation)` in test failures.
inline void PrintTo(const ResourceIdentity& identity, std::ostream* os) {
  *os << "(" << identity.slotIndex << ", " << identity.generation << ")";
}

}  // namespace donner::gpu

namespace donner::svg {

/// Prints retained composite entry counts by sequence in test failures.
inline void PrintTo(const RendererGeode::RetainedCompositeCounts& counts, std::ostream* os) {
  *os << "{layers: " << counts.layers << ", filterResults: " << counts.filterResults
      << ", images: " << counts.images << "}";
}

namespace {

/// Matches retained composite entry counts of \p layers isolated-layer and \p filterResults
/// filter-result entries.
testing::Matcher<RendererGeode::RetainedCompositeCounts> RetainedCounts(size_t layers,
                                                                        size_t filterResults) {
  return testing::AllOf(
      testing::Field("layers", &RendererGeode::RetainedCompositeCounts::layers, layers),
      testing::Field("filterResults", &RendererGeode::RetainedCompositeCounts::filterResults,
                     filterResults));
}

/// Matches retained composite entry counts of \p layers isolated-layer entries.
testing::Matcher<RendererGeode::RetainedCompositeCounts> RetainedLayerCount(size_t layers) {
  return testing::Field("layers", &RendererGeode::RetainedCompositeCounts::layers, layers);
}

/// Five isolated-layer composites over a background: three sibling opacity groups with distinct
/// opacities, one wrapping a nested group, and a `mix-blend-mode` composite. Every layer covers
/// the whole canvas, so all of them share one texture size.
constexpr std::string_view kLayerCompositeSvg = R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
  <rect width="64" height="64" fill="#204080"/>
  <g id="first" opacity="0.25"><rect x="4" y="4" width="24" height="24" fill="red"/></g>
  <g opacity="0.5"><rect x="36" y="4" width="24" height="24" fill="lime"/></g>
  <g opacity="0.75">
    <g opacity="0.6"><rect x="4" y="36" width="24" height="24" fill="yellow"/></g>
  </g>
  <rect x="36" y="36" width="24" height="24" fill="white" opacity="0.9"
        style="mix-blend-mode: multiply"/>
</svg>)svg";

/// Isolated layers composited by kLayerCompositeSvg.
constexpr size_t kLayerCompositeCount = 5;

/// Renders \p document through a fresh renderer on the shared device: the oracle for a renderer
/// whose composites kept state across earlier frames.
RendererBitmap RenderFresh(const std::shared_ptr<geode::GeodeDevice>& device,
                           SVGDocument& document) {
  RendererGeode fresh(device);
  fresh.draw(document);
  return fresh.takeSnapshot();
}

/// Matches a composite texture list in which every layer was composited.
MATCHER(AllComposited, "every layer composited from a texture") {
  return std::all_of(
      arg.begin(), arg.end(),
      [](const std::optional<gpu::ResourceIdentity>& identity) { return identity.has_value(); });
}

/// Each layer composites from the same texture on every unchanged frame, also when another renderer
/// on the device draws its own layers between frames, and from new ones once the layer size
/// changes; the bind groups follow the textures and the pixels stay those of a fresh renderer.
TEST_F(RendererGeodeTest, LayerCompositesKeepTheirTexturesAcrossUnchangedFrames) {
  std::optional<SVGDocument> document;
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(kLayerCompositeSvg, document));
  const RendererBitmap expected = RenderFresh(sharedDevice(), *document);

  RendererGeode renderer = createRenderer();
  renderer.draw(*document);
  const auto firstFrame = renderer.layerCompositeTexturesForTesting();
  ASSERT_THAT(firstFrame, testing::SizeIs(kLayerCompositeCount));
  ASSERT_THAT(firstFrame, AllComposited());

  RendererGeode other = createRenderer();
  for (int frame = 2; frame <= 3; ++frame) {
    SCOPED_TRACE(testing::Message() << "frame " << frame);
    if (frame == 3) {
      // The other renderer takes and returns textures of the same size before this frame.
      other.draw(*document);
      ASSERT_THAT(other.layerCompositeTexturesForTesting(), testing::SizeIs(kLayerCompositeCount));
    }
    renderer.draw(*document);
    EXPECT_THAT(renderer.layerCompositeTexturesForTesting(), testing::ElementsAreArray(firstFrame));
    const geode::GeodeCounters counters = renderer.lastFrameTimings().counters;
    EXPECT_EQ(counters.bindgroupCreates, 0u);
    EXPECT_EQ(counters.bufferWrites, 0u);
    editor::tests::CompareBitmapToBitmap(
        renderer.takeSnapshot(), expected,
        "layer_composites_unchanged_frame_" + std::to_string(frame),
        editor::tests::PixelmatchIdentityParams());
  }

  // A new canvas size gives every layer a texture of a new size.
  document->setCanvasSize(48, 48);
  const RendererBitmap expectedResized = RenderFresh(sharedDevice(), *document);
  renderer.draw(*document);
  const auto resizedFrame = renderer.layerCompositeTexturesForTesting();
  ASSERT_THAT(resizedFrame, testing::SizeIs(kLayerCompositeCount));
  ASSERT_THAT(resizedFrame, AllComposited());
  EXPECT_THAT(resizedFrame, testing::Pointwise(testing::Ne(), firstFrame))
      << "every layer must take a texture of the new size";
  EXPECT_EQ(renderer.lastFrameTimings().counters.bindgroupCreates, kLayerCompositeCount)
      << "every composite samples a new texture, so every bind group is rebuilt";
  editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), expectedResized,
                                       "layer_composites_resized_frame",
                                       editor::tests::PixelmatchIdentityParams());

  renderer.draw(*document);
  EXPECT_THAT(renderer.layerCompositeTexturesForTesting(), testing::ElementsAreArray(resizedFrame));
  EXPECT_EQ(renderer.lastFrameTimings().counters.bindgroupCreates, 0u);
  EXPECT_EQ(renderer.lastFrameTimings().counters.bufferWrites, 0u);
  editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), expectedResized,
                                       "layer_composites_after_resize",
                                       editor::tests::PixelmatchIdentityParams());
}

/// Destroying the pooled layer texture between frames frees its runtime slot, and the next frame's
/// new layer texture reuses that slot under a new generation. A composite keyed on the slot alone
/// would bind the destroyed texture; it must rebuild instead. One layer, so the slot the next frame
/// reuses is that layer's own.
TEST_F(RendererGeodeTest, LayerCompositeRebuildsWhenItsTextureWasDestroyed) {
  std::optional<SVGDocument> document;
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
        <rect width="64" height="64" fill="#204080"/>
        <g opacity="0.5"><rect x="8" y="8" width="40" height="40" fill="lime"/></g>
      </svg>)svg",
      document));
  const RendererBitmap expected = RenderFresh(sharedDevice(), *document);

  RendererGeode renderer = createRenderer();
  renderer.draw(*document);
  renderer.draw(*document);
  // The readback waits for the frame, so destroying its texture frees its slot for reuse.
  ASSERT_FALSE(renderer.takeSnapshot().empty());
  const auto beforeTrim = renderer.layerCompositeTexturesForTesting();
  ASSERT_THAT(beforeTrim, testing::SizeIs(1));
  ASSERT_THAT(beforeTrim, AllComposited());

  // The frame returned its layer texture to the pool; drop every free texture.
  renderer.trimTexturePool(RendererGeodeTexturePoolTrim::All);
  renderer.draw(*document);
  EXPECT_THAT(renderer.deviceLost(), testing::IsFalse());

  const auto afterTrim = renderer.layerCompositeTexturesForTesting();
  ASSERT_THAT(afterTrim, testing::SizeIs(1));
  ASSERT_THAT(afterTrim, AllComposited());
  EXPECT_THAT(afterTrim, testing::Pointwise(testing::Ne(), beforeTrim))
      << "the layer composited a destroyed texture";
  // Premise: the hazard this test guards needs the destroyed texture's slot handed to the layer's
  // new texture.
  EXPECT_THAT(afterTrim[0],
              testing::Optional(testing::Field("slotIndex", &gpu::ResourceIdentity::slotIndex,
                                               beforeTrim[0]->slotIndex)))
      << "premise: the destroyed texture's slot is reused by the layer's new texture";
  EXPECT_EQ(renderer.lastFrameTimings().counters.bindgroupCreates, 1u);
  editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), expected,
                                       "layer_composite_after_texture_destroyed",
                                       editor::tests::PixelmatchIdentityParams());
}

/// A second renderer on the device takes one of the first renderer's pooled layer textures, fills
/// it with its own submitted content, and still holds it while the first renderer draws again. The
/// first renderer's composites must not sample that texture through the bind groups they kept, and
/// every composite whose texture changed rebuilds its bind group.
TEST_F(RendererGeodeTest, LayerCompositeRebuildsWhenAnotherRendererHoldsItsTexture) {
  std::optional<SVGDocument> document;
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(kLayerCompositeSvg, document));
  const RendererBitmap expected = RenderFresh(sharedDevice(), *document);

  RendererGeode first = createRenderer();
  first.draw(*document);
  first.draw(*document);
  const auto firstFrames = first.layerCompositeTexturesForTesting();
  ASSERT_THAT(firstFrames, testing::SizeIs(kLayerCompositeCount));
  ASSERT_THAT(firstFrames, AllComposited());

  // The other renderer's layer takes the next texture of the size the pool hands out, one of the
  // first renderer's, paints it opaque red and submits.
  RendererGeode other = createRenderer();
  const auto paintOpaqueRedLayer = [&]() {
    RenderViewport viewport;
    viewport.size = Vector2d(64.0, 64.0);
    viewport.devicePixelRatio = 1.0;
    other.beginFrame(viewport);
    other.pushIsolatedLayer(1.0, MixBlendMode::Normal);
    other.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
    other.drawRect(Box2d({0, 0}, {64, 64}), StrokeParams{});
  };
  paintOpaqueRedLayer();
  other.popIsolatedLayer();
  other.endFrame();
  const auto taken = other.layerCompositeTexturesForTesting();
  ASSERT_THAT(taken, testing::SizeIs(1));
  ASSERT_THAT(taken, AllComposited());
  ASSERT_THAT(firstFrames, testing::Contains(taken[0]))
      << "premise: the other renderer took one of the first renderer's textures";

  // Its next frame takes the same texture back, which now holds its submitted red, and keeps it
  // while the first renderer draws.
  paintOpaqueRedLayer();
  first.draw(*document);
  EXPECT_THAT(first.deviceLost(), testing::IsFalse());
  const auto whileHeld = first.layerCompositeTexturesForTesting();
  ASSERT_THAT(whileHeld, testing::SizeIs(kLayerCompositeCount));
  ASSERT_THAT(whileHeld, AllComposited());
  EXPECT_THAT(whileHeld, testing::Not(testing::Contains(taken[0])))
      << "the first renderer composited a texture another renderer holds";
  const auto changed = static_cast<uint64_t>(std::count_if(
      whileHeld.begin(), whileHeld.end(),
      [&, i = size_t{0}](const std::optional<gpu::ResourceIdentity>& texture) mutable {
        return texture != firstFrames[i++];
      }));
  EXPECT_GT(changed, 0u);
  EXPECT_EQ(first.lastFrameTimings().counters.bindgroupCreates, changed)
      << "exactly the composites whose texture changed rebuild their bind groups";
  editor::tests::CompareBitmapToBitmap(first.takeSnapshot(), expected,
                                       "layer_composite_while_another_renderer_holds_texture",
                                       editor::tests::PixelmatchIdentityParams());

  other.popIsolatedLayer();
  other.endFrame();
  EXPECT_THAT(other.layerCompositeTexturesForTesting(), testing::ElementsAreArray(taken))
      << "premise: the other renderer held the taken texture through the first renderer's frame";
}

/// A filter result composites from the same output texture on every unchanged frame. Editing the
/// filtered content re-runs the filter into that same texture, so the composite keeps its bind
/// group and shows the new content. Covers both filter-result paths: a filter region covering the
/// whole canvas composites through a full-target blit, and a region-sized capture through a
/// textured quad at its offset. A composite whose texture changes is covered by the layer composite
/// tests, which share the binding.
TEST_F(RendererGeodeTest, FilterResultCompositeKeepsItsBindingAcrossContentEdits) {
  for (const auto& [name, region] :
       {std::pair<const char*, const char*>{
            "full_target", R"( filterUnits="userSpaceOnUse" x="0" y="0" width="64" height="64")"},
        std::pair<const char*, const char*>{"region_sized", ""}}) {
    SCOPED_TRACE(name);
    std::optional<SVGDocument> document;
    ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(
        std::string(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
          <filter id="b")svg") +
            region + R"svg(><feGaussianBlur stdDeviation="2"/></filter>
          <rect id="target" x="20" y="20" width="24" height="24" fill="green" filter="url(#b)"/>
        </svg>)svg",
        document));

    RendererGeode renderer = createRenderer();
    renderer.draw(*document);
    const auto firstFrame = renderer.filterOutputCompositeTexturesForTesting();
    ASSERT_THAT(firstFrame, testing::SizeIs(1));
    ASSERT_THAT(firstFrame, AllComposited());
    renderer.draw(*document);
    EXPECT_THAT(renderer.filterOutputCompositeTexturesForTesting(),
                testing::ElementsAreArray(firstFrame));
    EXPECT_EQ(renderer.lastFrameTimings().counters.bindgroupCreates, 0u);
    EXPECT_EQ(renderer.lastFrameTimings().counters.bufferWrites, 0u);
    editor::tests::CompareBitmapToBitmap(
        renderer.takeSnapshot(), RenderFresh(sharedDevice(), *document),
        std::string("filter_result_unchanged_") + name, editor::tests::PixelmatchIdentityParams());

    std::optional<SVGElement> target = document->querySelector("#target");
    ASSERT_THAT(target, testing::Optional(testing::_)) << "the fixture names this element";
    target->setAttribute("fill", "magenta");
    renderer.draw(*document);
    EXPECT_THAT(renderer.filterOutputCompositeTexturesForTesting(),
                testing::ElementsAreArray(firstFrame));
    EXPECT_EQ(renderer.lastFrameTimings().counters.bindgroupCreates, 0u)
        << "a content edit changes no bound resource of the composite";
    editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(),
                                         RenderFresh(sharedDevice(), *document),
                                         std::string("filter_result_content_edit_") + name,
                                         editor::tests::PixelmatchIdentityParams());
  }
}

/// A frame split by a memory-limited filter budget returns the surfaces of the layers it already
/// composited, so the frame admits more layers than its surface budget holds at once. The retained
/// composite entries stay capped, and the composites past the cap draw with per-frame state.
TEST_F(RendererGeodeTest, LayerCompositeRetainedStateIsCappedAcrossFrameSplits) {
  const size_t cap = RendererGeode::maxRetainedCompositesForTesting();
  constexpr size_t kLayersPerSegment = 200;
  constexpr int kExtent = 16;
  // Two segments of layers, split as a memory-limited filter budget splits a frame.
  const auto drawFrame = [&](RendererGeode& renderer) {
    RenderViewport viewport;
    viewport.size = Vector2d(kExtent, kExtent);
    viewport.devicePixelRatio = 1.0;
    renderer.beginFrame(viewport);
    for (size_t segment = 0; segment < 2; ++segment) {
      for (size_t i = 0; i < kLayersPerSegment; ++i) {
        const size_t pixel = (segment * kLayersPerSegment + i) % (kExtent * kExtent);
        renderer.pushIsolatedLayer(0.5, MixBlendMode::Normal);
        renderer.setPaint(
            solidFill(css::RGBA(static_cast<uint8_t>(i), 64, static_cast<uint8_t>(255 - i), 255)));
        renderer.drawRect(Box2d::FromXYWH(static_cast<double>(pixel % kExtent),
                                          static_cast<double>(pixel / kExtent), 1.0, 1.0),
                          StrokeParams{});
        renderer.popIsolatedLayer();
      }
      if (segment == 0) {
        (void)renderer.submitFilterBudgetChunkForTesting();
      }
    }
    renderer.endFrame();
  };
  // Premise: the frame composites more layers than the cap.
  ASSERT_GT(2 * kLayersPerSegment, cap);

  RendererGeode fresh = createRenderer();
  drawFrame(fresh);
  ASSERT_THAT(fresh.deviceLost(), testing::IsFalse());
  const RendererBitmap expected = fresh.takeSnapshot();

  RendererGeode renderer = createRenderer();
  for (int frame = 1; frame <= 2; ++frame) {
    SCOPED_TRACE(testing::Message() << "frame " << frame);
    drawFrame(renderer);
    ASSERT_THAT(renderer.deviceLost(), testing::IsFalse());
    // Exactly the cap: every layer of the frame was admitted, more than one segment's surfaces.
    EXPECT_THAT(renderer.retainedCompositeCountsForTesting(), RetainedLayerCount(cap));
    editor::tests::CompareBitmapToBitmap(
        renderer.takeSnapshot(), expected,
        "layer_composites_past_the_cap_frame_" + std::to_string(frame),
        editor::tests::PixelmatchIdentityParams());
  }
}

/// A renderer keeps composite entries only for what its last frame drew: drawing fewer layers
/// drops the rest once that frame is submitted, and trimming the texture pool to nothing releases
/// every entry.
TEST_F(RendererGeodeTest, LayerCompositeEntriesFollowTheLastFrame) {
  std::optional<SVGDocument> many;
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
        <filter id="b"><feGaussianBlur stdDeviation="2"/></filter>
        <g opacity="0.5"><rect width="20" height="20" fill="red"/></g>
        <g opacity="0.5"><rect x="20" width="20" height="20" fill="lime"/></g>
        <g opacity="0.5"><rect x="40" width="20" height="20" fill="blue"/></g>
        <rect y="30" width="20" height="20" fill="green" filter="url(#b)"/>
      </svg>)svg",
      many));
  std::optional<SVGDocument> few;
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
        <g opacity="0.5"><rect width="20" height="20" fill="red"/></g>
      </svg>)svg",
      few));

  RendererGeode renderer = createRenderer();
  renderer.draw(*many);
  EXPECT_THAT(renderer.retainedCompositeCountsForTesting(), RetainedCounts(3, 1));

  renderer.draw(*few);
  EXPECT_THAT(renderer.retainedCompositeCountsForTesting(), RetainedCounts(1, 0))
      << "entries the last frame did not reach must be dropped when it ends";

  renderer.trimTexturePool(RendererGeodeTexturePoolTrim::All);
  EXPECT_THAT(renderer.retainedCompositeCountsForTesting(), RetainedCounts(0, 0));

  renderer.draw(*many);
  EXPECT_THAT(renderer.deviceLost(), testing::IsFalse());
  EXPECT_THAT(renderer.retainedCompositeCountsForTesting(), RetainedLayerCount(3));
  editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), RenderFresh(sharedDevice(), *many),
                                       "layer_composite_entries_after_trim",
                                       editor::tests::PixelmatchIdentityParams());
}

/// Another renderer on the same device can make the shared texture pool destroy the textures this
/// renderer's retained composites were built from, by trimming the pool or by beginning frames
/// until their bucket goes stale. Each entry must let go of its texture views and bind group before
/// the pool destroys a texture they name, keeping only its uniform buffer, and the next frame must
/// rebuild them from live textures. Ends with both renderers torn down, which the sanitizer lanes
/// check for use after free.
TEST_F(RendererGeodeTest, RetainedCompositesReleaseViewsBeforeThePoolDestroysTheirTextures) {
  std::optional<SVGDocument> document;
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
        <filter id="b"><feGaussianBlur stdDeviation="2"/></filter>
        <g opacity="0.5"><rect width="20" height="20" fill="red"/></g>
        <g opacity="0.5"><rect x="20" width="20" height="20" fill="lime"/></g>
        <g opacity="0.5"><rect x="40" width="20" height="20" fill="blue"/></g>
        <rect y="30" width="20" height="20" fill="green" filter="url(#b)"/>
      </svg>)svg",
      document));
  std::optional<SVGDocument> plain;
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32">
        <rect width="16" height="16" fill="red"/>
      </svg>)svg",
      plain));

  // A device of its own, so the pool holds only what these renderers put there.
  const std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
  ASSERT_THAT(device, testing::NotNull());
  RendererGeode renderer(device);
  RendererGeode other(device);

  const auto expectRebuiltFromLiveTextures = [&](const char* label) {
    renderer.draw(*document);
    EXPECT_THAT(renderer.deviceLost(), testing::IsFalse()) << label;
    EXPECT_THAT(renderer.retainedCompositeViewHoldersForTesting(), RetainedCounts(3, 1)) << label;
    const RendererBitmap retained = renderer.takeSnapshot();
    editor::tests::CompareBitmapToBitmap(retained, RenderFresh(device, *document), label,
                                         editor::tests::PixelmatchIdentityParams());
  };

  renderer.draw(*document);
  ASSERT_THAT(renderer.retainedCompositeViewHoldersForTesting(), RetainedCounts(3, 1));
  ASSERT_THAT(renderer.texturePoolStats().textureCount, testing::Gt(0u))
      << "the frame's layer and filter textures must be back in the pool";

  other.trimTexturePool(RendererGeodeTexturePoolTrim::All);
  ASSERT_THAT(renderer.texturePoolStats().textureCount, testing::Eq(0u));
  EXPECT_THAT(renderer.retainedCompositeViewHoldersForTesting(), RetainedCounts(0, 0))
      << "a trim by another renderer destroyed the textures these entries viewed";
  EXPECT_THAT(renderer.retainedCompositeCountsForTesting(), RetainedCounts(3, 1))
      << "entries keep their uniform buffers";
  expectRebuiltFromLiveTextures("retained_composites_after_other_renderer_trim");

  // The pool evicts a bucket no frame has used for 120 frames, counting every renderer's frames.
  constexpr int kFramesPastStale = 125;
  for (int frame = 0; frame < kFramesPastStale; ++frame) {
    other.draw(*plain);
  }
  EXPECT_THAT(renderer.retainedCompositeViewHoldersForTesting(), RetainedCounts(0, 0))
      << "stale-bucket eviction destroyed the textures these entries viewed";
  expectRebuiltFromLiveTextures("retained_composites_after_stale_eviction");

  // A later frame of larger textures evicts the least recently used buckets to fit the pool's
  // budget: six nested full-canvas layers of 16 MiB each return 96 MiB at once, past the 64 MiB
  // budget a pool of small frames keeps.
  std::optional<SVGDocument> large;
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="2048" height="2048">
        <g opacity="0.9"><rect width="2048" height="2048" fill="red"/>
         <g opacity="0.9"><rect width="2048" height="2048" fill="lime"/>
          <g opacity="0.9"><rect width="2048" height="2048" fill="blue"/>
           <g opacity="0.9"><rect width="2048" height="2048" fill="yellow"/>
            <g opacity="0.9"><rect width="2048" height="2048" fill="white"/>
             <g opacity="0.9"><rect width="2048" height="2048" fill="black"/></g>
            </g>
           </g>
          </g>
         </g>
        </g>
      </svg>)svg",
      large));
  other.draw(*large);
  ASSERT_THAT(other.deviceLost(), testing::IsFalse());
  EXPECT_THAT(renderer.retainedCompositeViewHoldersForTesting(), RetainedCounts(0, 0))
      << "budget eviction destroyed the textures these entries viewed";
  expectRebuiltFromLiveTextures("retained_composites_after_budget_eviction");
}

/// A frame that ends without submitting, whether it never reaches endFrame or its recording fails,
/// releases every retained entry's views and bind group: the textures it touched are retained or
/// destroyed outside the pool, and its clip mask views go with it. The next frame rebuilds them.
TEST_F(RendererGeodeTest, RetainedCompositesReleaseViewsWhenAFrameIsAbandoned) {
  std::optional<SVGDocument> document;
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
        <filter id="b"><feGaussianBlur stdDeviation="2"/></filter>
        <g opacity="0.5"><rect width="20" height="20" fill="red"/></g>
        <g opacity="0.5"><rect x="20" width="20" height="20" fill="lime"/></g>
        <g opacity="0.5"><rect x="40" width="20" height="20" fill="blue"/></g>
        <rect y="30" width="20" height="20" fill="green" filter="url(#b)"/>
      </svg>)svg",
      document));

  const std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
  ASSERT_THAT(device, testing::NotNull());
  RendererGeode renderer(device);
  // Claims the first layer entry of the frame and composites through it.
  const auto compositeOneLayer = [&] {
    renderer.pushIsolatedLayer(0.5, MixBlendMode::Normal);
    renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
    renderer.drawRect(Box2d({0, 0}, {20, 20}), StrokeParams{});
    renderer.popIsolatedLayer();
  };

  renderer.draw(*document);
  ASSERT_THAT(renderer.retainedCompositeViewHoldersForTesting(), RetainedCounts(3, 1));
  beginFrame(renderer);
  compositeOneLayer();
  beginFrame(renderer);
  EXPECT_THAT(renderer.retainedCompositeViewHoldersForTesting(), RetainedCounts(0, 0))
      << "a frame that never reached endFrame must release every entry's views";
  renderer.endFrame();

  renderer.draw(*document);
  ASSERT_THAT(renderer.retainedCompositeViewHoldersForTesting(), RetainedCounts(3, 1));
  beginFrame(renderer);
  compositeOneLayer();
  renderer.injectActiveArenaWriteFailureForTesting();
  renderer.setPaint(solidFill(css::RGBA(0, 0, 255, 255)));
  renderer.drawRect(Box2d({0, 0}, {kViewportSize, kViewportSize}), StrokeParams{});
  renderer.endFrame();
  EXPECT_THAT(renderer.deviceLost(), testing::IsFalse());
  EXPECT_THAT(renderer.retainedCompositeViewHoldersForTesting(), RetainedCounts(0, 0))
      << "a frame abandoned at endFrame must release every entry's views";

  // The failed frame's textures are destroyed once the submissions before it complete; until then
  // the next frame is refused, so wait for the queue as a host would.
  ASSERT_THAT(device->waitForQueueIdle(), testing::Eq(geode::GpuWaitResult::Complete));
  renderer.draw(*document);
  EXPECT_THAT(renderer.deviceLost(), testing::IsFalse());
  EXPECT_THAT(renderer.retainedCompositeViewHoldersForTesting(), RetainedCounts(3, 1));
  const RendererBitmap recovered = renderer.takeSnapshot();
  editor::tests::CompareBitmapToBitmap(recovered, RenderFresh(device, *document),
                                       "retained_composites_after_abandoned_frames",
                                       editor::tests::PixelmatchIdentityParams());
}

/// The views a renderer keeps of pattern tile and clip-mask textures go with those textures.
/// Another renderer on the same device makes the shared pool destroy them, by trimming it, by
/// beginning frames until their bucket goes stale, and by returning textures past the pool's
/// budget, and a frame that ends without submitting drops them too. Each time the renderer keeps no
/// view, and its next frame rebuilds them from live textures and paints what a fresh renderer
/// paints. Ends with both renderers torn down, which the sanitizer lanes check for use after free.
TEST_F(RendererGeodeTest, TileAndClipMaskViewsGoWithTheirTextures) {
  std::optional<SVGDocument> document;
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
        <pattern id="p" patternUnits="userSpaceOnUse" width="8" height="8">
          <rect width="4" height="4" fill="#208040"/>
        </pattern>
        <clipPath id="c"><circle cx="32" cy="32" r="24"/></clipPath>
        <rect width="64" height="64" fill="url(#p)" clip-path="url(#c)"/>
      </svg>)svg",
      document));
  std::optional<SVGDocument> plain;
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32">
        <rect width="16" height="16" fill="red"/>
      </svg>)svg",
      plain));
  // Six nested full-canvas layers of 16 MiB each return 96 MiB at once, past the 64 MiB budget a
  // pool of small frames keeps, so the pool evicts its least recently used buckets.
  std::optional<SVGDocument> large;
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="2048" height="2048">
        <g opacity="0.9"><rect width="2048" height="2048" fill="red"/>
         <g opacity="0.9"><rect width="2048" height="2048" fill="lime"/>
          <g opacity="0.9"><rect width="2048" height="2048" fill="blue"/>
           <g opacity="0.9"><rect width="2048" height="2048" fill="yellow"/>
            <g opacity="0.9"><rect width="2048" height="2048" fill="white"/>
             <g opacity="0.9"><rect width="2048" height="2048" fill="black"/></g>
            </g>
           </g>
          </g>
         </g>
        </g>
      </svg>)svg",
      large));

  // A device of its own, so the pool holds only what these renderers put there.
  const std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
  ASSERT_THAT(device, testing::NotNull());
  RendererGeode renderer(device);
  RendererGeode other(device);
  const auto keptViews = [&] {
    return std::make_pair(renderer.patternTileViewCountForTesting(),
                          renderer.clipMaskViewCountForTesting());
  };
  const auto drawAndExpectRebuilt = [&](const char* label) {
    renderer.draw(*document);
    EXPECT_THAT(renderer.deviceLost(), testing::IsFalse()) << label;
    EXPECT_THAT(keptViews(), testing::Pair(testing::Gt(0u), testing::Gt(0u)))
        << label << ": the frame keeps views of the textures it drew";
    editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), RenderFresh(device, *document),
                                         label, editor::tests::PixelmatchIdentityParams());
  };

  drawAndExpectRebuilt("tile_and_clip_views_first_frame");

  other.trimTexturePool(RendererGeodeTexturePoolTrim::All);
  EXPECT_THAT(keptViews(), testing::Pair(testing::Eq(0u), testing::Eq(0u)))
      << "a trim by another renderer destroyed the textures these views viewed";
  drawAndExpectRebuilt("tile_and_clip_views_after_other_renderer_trim");

  // The pool evicts a bucket no frame has used for 120 frames, counting every renderer's frames.
  constexpr int kFramesPastStale = 125;
  for (int frame = 0; frame < kFramesPastStale; ++frame) {
    other.draw(*plain);
  }
  EXPECT_THAT(keptViews(), testing::Pair(testing::Eq(0u), testing::Eq(0u)))
      << "stale-bucket eviction destroyed the textures these views viewed";
  drawAndExpectRebuilt("tile_and_clip_views_after_stale_eviction");

  other.draw(*large);
  ASSERT_THAT(other.deviceLost(), testing::IsFalse());
  EXPECT_THAT(keptViews(), testing::Pair(testing::Eq(0u), testing::Eq(0u)))
      << "budget eviction destroyed the textures these views viewed";
  drawAndExpectRebuilt("tile_and_clip_views_after_budget_eviction");

  // A frame that never reaches endFrame submits nothing, and the next frame drops every view.
  beginFrame(renderer);
  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  renderer.drawRect(Box2d({0, 0}, {20, 20}), StrokeParams{});
  beginFrame(renderer);
  EXPECT_THAT(keptViews(), testing::Pair(testing::Eq(0u), testing::Eq(0u)))
      << "a frame that never reached endFrame must drop every kept view";
  renderer.endFrame();
  drawAndExpectRebuilt("tile_and_clip_views_after_abandoned_frame");
}

/// Edits that change what a retained composite draws with, each compared with a fresh renderer:
/// a blend mode, a filter region that moves the same filter result between the full-target blit
/// and the offset quad, a canvas resize that rescales the document through its viewBox, and a
/// composite under a path clip mask, whose binding names the clip mask's retained view.
TEST_F(RendererGeodeTest, CompositeEditsMatchAFreshRenderer) {
  std::optional<SVGDocument> document;
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64" viewBox="0 0 64 64">
        <clipPath id="c"><circle cx="16" cy="48" r="12"/></clipPath>
        <filter id="b" filterUnits="userSpaceOnUse" x="0" y="0" width="64" height="64">
          <feGaussianBlur stdDeviation="2"/>
        </filter>
        <rect width="64" height="64" fill="#204080"/>
        <rect id="blend" x="4" y="4" width="24" height="24" fill="orange"
              style="mix-blend-mode: multiply"/>
        <rect x="36" y="4" width="24" height="24" fill="green" filter="url(#b)"/>
        <g opacity="0.6" clip-path="url(#c)"><rect x="0" y="32" width="32" height="32" fill="red"/></g>
      </svg>)svg",
      document));

  RendererGeode renderer = createRenderer();
  const auto expectFresh = [&](const char* label) {
    renderer.draw(*document);
    EXPECT_THAT(renderer.deviceLost(), testing::IsFalse()) << label;
    editor::tests::CompareBitmapToBitmap(
        renderer.takeSnapshot(), RenderFresh(sharedDevice(), *document),
        std::string("composite_edit_") + label, editor::tests::PixelmatchIdentityParams());
  };
  expectFresh("first");
  expectFresh("unchanged_with_clip_mask");
  expectFresh("unchanged_with_clip_mask_again");

  std::optional<SVGElement> blend = document->querySelector("#blend");
  ASSERT_THAT(blend, testing::Optional(testing::_)) << "the fixture names this element";
  blend->setAttribute("style", "mix-blend-mode: screen");
  expectFresh("blend_mode");

  std::optional<SVGElement> filter = document->querySelector("#b");
  ASSERT_THAT(filter, testing::Optional(testing::_)) << "the fixture names this element";
  filter->setAttribute("filterUnits", "objectBoundingBox");
  filter->setAttribute("x", "-0.1");
  filter->setAttribute("y", "-0.1");
  filter->setAttribute("width", "1.2");
  filter->setAttribute("height", "1.2");
  expectFresh("filter_region_to_quad");
  filter->setAttribute("filterUnits", "userSpaceOnUse");
  filter->setAttribute("x", "0");
  filter->setAttribute("y", "0");
  filter->setAttribute("width", "64");
  filter->setAttribute("height", "64");
  expectFresh("filter_region_to_blit");

  document->setCanvasSize(96, 96);
  expectFresh("canvas_rescale");
}

/// Editing a layer's opacity rewrites that composite's uniform and nothing else: its textures and
/// bind group are unchanged.
TEST_F(RendererGeodeTest, LayerCompositeRewritesItsUniformWhenOpacityChanges) {
  std::optional<SVGDocument> document;
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(kLayerCompositeSvg, document));

  RendererGeode renderer = createRenderer();
  renderer.draw(*document);
  renderer.draw(*document);

  std::optional<SVGElement> group = document->querySelector("#first");
  ASSERT_THAT(group, testing::Optional(testing::_)) << "the fixture names this element";
  group->setAttribute("opacity", "0.4");
  const RendererBitmap expected = RenderFresh(sharedDevice(), *document);

  renderer.draw(*document);
  const geode::GeodeCounters edited = renderer.lastFrameTimings().counters;
  EXPECT_EQ(edited.bufferWrites, 1u) << "the edited composite rewrites its uniform once";
  EXPECT_EQ(edited.bindgroupCreates, 0u) << "an opacity edit changes no bound resource";
  editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), expected,
                                       "layer_composite_opacity_edit",
                                       editor::tests::PixelmatchIdentityParams());

  renderer.draw(*document);
  EXPECT_EQ(renderer.lastFrameTimings().counters.bufferWrites, 0u);
  EXPECT_EQ(renderer.lastFrameTimings().counters.bindgroupCreates, 0u);
}

/// A composite keeps state only for a layer texture the frame's surface budget admitted. Nested
/// layers each hold their surface until they pop, so a nest deeper than the budget allows keeps at
/// most one entry per admitted surface, and those stay steady.
TEST_F(RendererGeodeTest, LayerCompositeRetainedStateFollowsTheSurfaceBudget) {
  constexpr size_t kDepth = 12;
  std::string source = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="20" height="15">)svg";
  for (size_t i = 0; i < kDepth; ++i) {
    source += "<g opacity=\"0.9\"><rect x=\"" + std::to_string(i) +
              R"svg(" width="1" height="1" fill="black"/>)svg";
  }
  for (size_t i = 0; i < kDepth; ++i) {
    source += "</g>";
  }
  source += "</svg>";
  std::optional<SVGDocument> document;
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(source, document));

  RendererGeode renderer = createRenderer();
  renderer.setSurfaceBudgetForTesting(/*maximumSurfaces=*/8u, /*maximumBytes=*/1u << 20);
  renderer.draw(*document);
  renderer.draw(*document);
  // The frame's own target takes one surface.
  EXPECT_THAT(renderer.layerCompositeTexturesForTesting(), testing::SizeIs(7u));
  const geode::GeodeCounters counters = renderer.lastFrameTimings().counters;
  EXPECT_EQ(counters.bufferWrites, 0u);
  EXPECT_EQ(counters.bindgroupCreates, 0u);

  renderer.setSurfaceBudgetForTesting(/*maximumSurfaces=*/4u, /*maximumBytes=*/1u << 20);
  renderer.draw(*document);
  EXPECT_THAT(renderer.layerCompositeTexturesForTesting(), testing::SizeIs(3u));
}

/// Sibling layers take turns with one surface, so the surface budget admits every one of them,
/// however many there are. The composites past the retained-composite cap draw with per-frame
/// state.
TEST_F(RendererGeodeTest, SiblingLayersPastTheSurfaceBudgetAreAllAdmitted) {
  constexpr size_t kLayers = 300;
  std::string source = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="20" height="15">)svg";
  for (size_t i = 0; i < kLayers; ++i) {
    source += "<rect x=\"" + std::to_string(i % 20) + "\" y=\"" + std::to_string(i / 20) +
              R"svg(" width="1" height="1" fill="black" opacity="0.5"/>)svg";
  }
  source += "</svg>";
  std::optional<SVGDocument> document;
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(source, document));
  const size_t cap = RendererGeode::maxRetainedCompositesForTesting();
  ASSERT_GT(kLayers, cap);

  RendererGeode renderer = createRenderer();
  renderer.setSurfaceBudgetForTesting(/*maximumSurfaces=*/4u, /*maximumBytes=*/1u << 20);
  renderer.draw(*document);
  renderer.draw(*document);
  EXPECT_THAT(renderer.layerCompositeTexturesForTesting(), testing::SizeIs(cap));
  const geode::GeodeCounters counters = renderer.lastFrameTimings().counters;
  EXPECT_THAT(counters.bufferWrites, testing::Eq(kLayers - cap))
      << "each composite past the cap writes its own uniform";
  EXPECT_THAT(counters.bindgroupCreates, testing::Eq(kLayers - cap))
      << "each composite past the cap builds its own bind group";

  // Every layer composites at its opacity: the squares do not overlap, so one group at the same
  // opacity around all of them renders the same pixels.
  std::string grouped = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="20" height="15">
      <g opacity="0.5">)svg";
  for (size_t i = 0; i < kLayers; ++i) {
    grouped += "<rect x=\"" + std::to_string(i % 20) + "\" y=\"" + std::to_string(i / 20) +
               R"svg(" width="1" height="1" fill="black"/>)svg";
  }
  grouped += "</g></svg>";
  std::optional<SVGDocument> groupedDocument;
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(grouped, groupedDocument));
  editor::tests::CompareBitmapToBitmap(
      renderer.takeSnapshot(), RenderFresh(sharedDevice(), *groupedDocument),
      "sibling_layers_past_the_surface_budget", editor::tests::PixelmatchIdentityParams());
}

/// The surface budget also bounds bytes, and a refused reservation latches for the rest of the
/// frame. Sibling layers, masks and clips whose surfaces would together pass the byte limit, while
/// their count stays far under the surface limit, take turns with a few surfaces, so they render
/// what they render under a budget that cannot bind.
TEST_F(RendererGeodeTest, SiblingSurfacesPastTheSurfaceByteBudgetRenderAsUnbudgeted) {
  constexpr std::uint64_t kBudgetBytes = 8 * kSiblingSurfaceBytes;
  SVGDocument document = ParseDocumentOrFail(SiblingGroupsSource(24, [](int i) {
    switch (i % 3) {
      case 0: return std::string(R"(<g opacity="0.7">)");
      case 1: return std::string("<g mask=\"url(#m)\">");
      default: return std::string("<g clip-path=\"url(#c)\">");
    }
  }));

  RendererGeode unbudgeted = createRenderer();
  unbudgeted.setWithinFrameTextureReuseForTesting(false);
  unbudgeted.setSurfaceBudgetForTesting(/*maximumSurfaces=*/1024u, /*maximumBytes=*/1u << 30);
  unbudgeted.draw(document);
  ASSERT_THAT(unbudgeted.resourceStats().surfacePeakBytes, testing::Gt(kBudgetBytes))
      << "one surface per use must pass the byte limit for this case to reach it";

  RendererGeode budgeted = createRenderer();
  budgeted.setSurfaceBudgetForTesting(RendererSurfaceBudget::kMaximumSurfaces, kBudgetBytes);
  budgeted.draw(document);
  EXPECT_THAT(budgeted.resourceStats().surfaceBudgetRejected, testing::IsFalse())
      << "peak " << budgeted.resourceStats().surfacePeakBytes << " of " << kBudgetBytes;
  editor::tests::CompareBitmapToBitmap(budgeted.takeSnapshot(), unbudgeted.takeSnapshot(),
                                       "sibling_surfaces_past_the_byte_budget",
                                       editor::tests::PixelmatchIdentityParams());
}

/// Matches retained composite counts of \p masks `<mask>` composite entries.
testing::Matcher<RendererGeode::RetainedCompositeCounts> RetainedMaskCount(size_t masks) {
  return testing::Field("masks", &RendererGeode::RetainedCompositeCounts::masks, masks);
}

/// Four `<mask>` composites over a background: a luminance mask, an alpha mask with user-space
/// bounds, and a mask whose content is masked by a second mask. Every mask covers the whole canvas,
/// so all of their textures share one size.
constexpr std::string_view kMaskCompositeSvg = R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
  <mask id="luminance">
    <rect width="64" height="64" fill="white"/>
    <circle id="hole" cx="16" cy="16" r="9" fill="#202020"/>
  </mask>
  <mask id="alpha" mask-type="alpha" maskUnits="userSpaceOnUse" x="34" y="2" width="28"
        height="28">
    <rect width="64" height="64" fill="black" fill-opacity="0.6"/>
  </mask>
  <mask id="inner"><rect x="2" y="34" width="14" height="28" fill="white"/></mask>
  <mask id="outer"><rect x="2" y="34" width="28" height="28" fill="white" mask="url(#inner)"/></mask>
  <rect width="64" height="64" fill="#204080"/>
  <rect id="luminance-target" x="2" y="2" width="28" height="28" fill="orange"
        mask="url(#luminance)"/>
  <rect id="alpha-target" x="34" y="2" width="28" height="28" fill="lime" mask="url(#alpha)"/>
  <rect x="2" y="34" width="28" height="28" fill="red" mask="url(#outer)"/>
</svg>)svg";

/// `<mask>` composites drawn by kMaskCompositeSvg, the nested mask's included.
constexpr size_t kMaskCompositeCount = 4;

/// Each mask composites from the same textures on every unchanged frame, also when another renderer
/// on the device draws its own masks between frames, so it writes no uniform and creates no bind
/// group; once the canvas size changes the composites take textures of the new size and are steady
/// again on the next frame. The pixels stay those of a fresh renderer throughout.
TEST_F(RendererGeodeTest, MaskCompositesKeepTheirBindingsAcrossUnchangedFrames) {
  std::optional<SVGDocument> document;
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(kMaskCompositeSvg, document));
  const RendererBitmap expected = RenderFresh(sharedDevice(), *document);

  RendererGeode renderer = createRenderer();
  renderer.draw(*document);
  const auto firstFrame = renderer.maskCompositeTexturesForTesting();
  ASSERT_THAT(firstFrame, testing::SizeIs(kMaskCompositeCount));
  ASSERT_THAT(firstFrame, AllComposited());

  RendererGeode other = createRenderer();
  for (int frame = 2; frame <= 3; ++frame) {
    SCOPED_TRACE(testing::Message() << "frame " << frame);
    if (frame == 3) {
      // The other renderer takes and returns textures of the same size before this frame.
      other.draw(*document);
      ASSERT_THAT(other.maskCompositeTexturesForTesting(), testing::SizeIs(kMaskCompositeCount));
    }
    renderer.draw(*document);
    EXPECT_THAT(renderer.maskCompositeTexturesForTesting(), testing::ElementsAreArray(firstFrame));
    const geode::GeodeCounters counters = renderer.lastFrameTimings().counters;
    EXPECT_EQ(counters.bindgroupCreates, 0u);
    EXPECT_EQ(counters.bufferWrites, 0u);
    editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), expected,
                                         "mask_composites_unchanged_frame_" + std::to_string(frame),
                                         editor::tests::PixelmatchIdentityParams());
  }

  document->setCanvasSize(48, 48);
  const RendererBitmap expectedResized = RenderFresh(sharedDevice(), *document);
  renderer.draw(*document);
  const auto resizedFrame = renderer.maskCompositeTexturesForTesting();
  ASSERT_THAT(resizedFrame, testing::SizeIs(kMaskCompositeCount));
  ASSERT_THAT(resizedFrame, AllComposited());
  EXPECT_THAT(resizedFrame, testing::Pointwise(testing::Ne(), firstFrame))
      << "every mask must take textures of the new size";
  editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), expectedResized,
                                       "mask_composites_resized_frame",
                                       editor::tests::PixelmatchIdentityParams());

  renderer.draw(*document);
  EXPECT_THAT(renderer.maskCompositeTexturesForTesting(), testing::ElementsAreArray(resizedFrame));
  EXPECT_EQ(renderer.lastFrameTimings().counters.bindgroupCreates, 0u);
  EXPECT_EQ(renderer.lastFrameTimings().counters.bufferWrites, 0u);
  editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), expectedResized,
                                       "mask_composites_after_resize",
                                       editor::tests::PixelmatchIdentityParams());
}

/// Edits that change what a retained mask composite draws, each compared with a fresh renderer
/// after a warm renderer has drawn the document twice: the mask's content, the masked content, the
/// mask's color interpolation, a mask removed and restored, which shifts the composites after it to
/// other entries, the mask's type, and its region. Each edit must change the picture, or a stale
/// binding would pass unnoticed.
TEST_F(RendererGeodeTest, MaskCompositeEditsMatchAFreshRenderer) {
  std::optional<SVGDocument> document;
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(kMaskCompositeSvg, document));
  std::optional<SVGElement> hole = document->querySelector("#hole");
  std::optional<SVGElement> alphaTarget = document->querySelector("#alpha-target");
  std::optional<SVGElement> luminance = document->querySelector("#luminance");
  std::optional<SVGElement> luminanceTarget = document->querySelector("#luminance-target");
  ASSERT_THAT(hole, testing::Optional(testing::_)) << "the fixture names this element";
  ASSERT_THAT(alphaTarget, testing::Optional(testing::_)) << "the fixture names this element";
  ASSERT_THAT(luminance, testing::Optional(testing::_)) << "the fixture names this element";
  ASSERT_THAT(luminanceTarget, testing::Optional(testing::_)) << "the fixture names this element";
  struct Edit {
    const char* name;
    std::function<void()> apply;
  };
  const std::vector<Edit> edits = {
      {"mask_content", [&] { hole->setAttribute("fill", "#a0a0a0"); }},
      {"masked_content", [&] { alphaTarget->setAttribute("fill", "magenta"); }},
      {"color_interpolation", [&] { luminance->setAttribute("color-interpolation", "linearRGB"); }},
      {"mask_removed", [&] { luminanceTarget->removeAttribute("mask"); }},
      {"mask_restored", [&] { luminanceTarget->setAttribute("mask", "url(#luminance)"); }},
      {"mask_type", [&] { luminance->setAttribute("mask-type", "alpha"); }},
      // The luminance mask's region is the default, relative to the masked element's bounds, so
      // resizing the element moves the region the composite is bounded by.
      {"mask_region", [&] { luminanceTarget->setAttribute("width", "20"); }},
  };

  RendererGeode renderer = createRenderer();
  renderer.draw(*document);
  for (const Edit& edit : edits) {
    SCOPED_TRACE(edit.name);
    renderer.draw(*document);
    const RendererBitmap beforeEdit = renderer.takeSnapshot();
    edit.apply();
    const RendererBitmap expected = RenderFresh(sharedDevice(), *document);
    // Premise, measured with an unbounded pixel budget: the edit changes the rendering.
    editor::tests::BitmapGoldenCompareParams signalParams =
        editor::tests::PixelmatchIdentityParams();
    signalParams.maxMismatchedPixels = std::numeric_limits<int>::max();
    int editedPixels = -1;
    editor::tests::CompareBitmapToBitmap(beforeEdit, expected,
                                         std::string("mask_edit_premise_") + edit.name,
                                         signalParams, &editedPixels);
    ASSERT_THAT(editedPixels, testing::Gt(0))
        << "Premise: the edit must change the rendering, or a stale binding would pass unnoticed";

    renderer.draw(*document);
    EXPECT_THAT(renderer.deviceLost(), testing::IsFalse());
    editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), expected,
                                         std::string("mask_edit_") + edit.name,
                                         editor::tests::PixelmatchIdentityParams());
  }
}

/// The views and bind groups a renderer keeps for its mask composites, and for composites under a
/// path clip, go with the textures and clip-mask views they name. Another renderer on the same
/// device makes the shared pool destroy those textures, by trimming it, by beginning frames until
/// their bucket goes stale, and by returning textures past the pool's budget, and a frame that ends
/// without submitting releases them too. Each time no entry holds a view, and the next frame
/// rebuilds them from live textures and paints what a fresh renderer paints. Ends with both
/// renderers torn down, which the sanitizer lanes check for use after free.
TEST_F(RendererGeodeTest, MaskAndClippedCompositesReleaseViewsWithTheirTextures) {
  std::optional<SVGDocument> document;
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
        <mask id="m"><rect width="64" height="64" fill="#a0a0a0"/></mask>
        <clipPath id="c"><circle cx="48" cy="48" r="14"/></clipPath>
        <rect width="32" height="32" fill="orange" mask="url(#m)"/>
        <g opacity="0.5" clip-path="url(#c)"><rect x="32" y="32" width="32" height="32" fill="red"/></g>
      </svg>)svg",
      document));
  std::optional<SVGDocument> plain;
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32">
        <rect width="16" height="16" fill="red"/>
      </svg>)svg",
      plain));
  // Six nested full-canvas layers of 16 MiB each return 96 MiB at once, past the 64 MiB budget a
  // pool of small frames keeps, so the pool evicts its least recently used buckets.
  std::optional<SVGDocument> large;
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="2048" height="2048">
        <g opacity="0.9"><rect width="2048" height="2048" fill="red"/>
         <g opacity="0.9"><rect width="2048" height="2048" fill="lime"/>
          <g opacity="0.9"><rect width="2048" height="2048" fill="blue"/>
           <g opacity="0.9"><rect width="2048" height="2048" fill="yellow"/>
            <g opacity="0.9"><rect width="2048" height="2048" fill="white"/>
             <g opacity="0.9"><rect width="2048" height="2048" fill="black"/></g>
            </g>
           </g>
          </g>
         </g>
        </g>
      </svg>)svg",
      large));

  // A device of its own, so the pool holds only what these renderers put there.
  const std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
  ASSERT_THAT(device, testing::NotNull());
  RendererGeode renderer(device);
  RendererGeode other(device);
  const auto holdsNoViews = testing::AllOf(RetainedLayerCount(0), RetainedMaskCount(0));
  const auto drawAndExpectRebuilt = [&](const char* label) {
    renderer.draw(*document);
    EXPECT_THAT(renderer.deviceLost(), testing::IsFalse()) << label;
    EXPECT_THAT(renderer.retainedCompositeViewHoldersForTesting(),
                testing::AllOf(RetainedLayerCount(1), RetainedMaskCount(1)))
        << label << ": the frame's composites hold views of what they drew";
    editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), RenderFresh(device, *document),
                                         label, editor::tests::PixelmatchIdentityParams());
  };

  drawAndExpectRebuilt("mask_and_clipped_first_frame");
  // The fresh rendering above drew on this device too, so the next frame may take its pooled
  // textures in another order; the frame after it is unchanged.
  renderer.draw(*document);
  renderer.draw(*document);
  ASSERT_EQ(renderer.lastFrameTimings().counters.bindgroupCreates, 0u)
      << "premise: an unchanged frame reuses both composites' bind groups";

  other.trimTexturePool(RendererGeodeTexturePoolTrim::All);
  EXPECT_THAT(renderer.retainedCompositeViewHoldersForTesting(), holdsNoViews)
      << "a trim by another renderer destroyed the textures these entries viewed";
  EXPECT_THAT(renderer.retainedCompositeCountsForTesting(),
              testing::AllOf(RetainedLayerCount(1), RetainedMaskCount(1)))
      << "entries keep their uniform buffers";
  drawAndExpectRebuilt("mask_and_clipped_after_other_renderer_trim");

  // The pool evicts a bucket no frame has used for 120 frames, counting every renderer's frames.
  constexpr int kFramesPastStale = 125;
  for (int frame = 0; frame < kFramesPastStale; ++frame) {
    other.draw(*plain);
  }
  EXPECT_THAT(renderer.retainedCompositeViewHoldersForTesting(), holdsNoViews)
      << "stale-bucket eviction destroyed the textures these entries viewed";
  drawAndExpectRebuilt("mask_and_clipped_after_stale_eviction");

  other.draw(*large);
  ASSERT_THAT(other.deviceLost(), testing::IsFalse());
  EXPECT_THAT(renderer.retainedCompositeViewHoldersForTesting(), holdsNoViews)
      << "budget eviction destroyed the textures these entries viewed";
  drawAndExpectRebuilt("mask_and_clipped_after_budget_eviction");

  // A frame that never reaches endFrame submits nothing, and the next frame releases every view.
  beginFrame(renderer);
  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  renderer.drawRect(Box2d({0, 0}, {20, 20}), StrokeParams{});
  beginFrame(renderer);
  EXPECT_THAT(renderer.retainedCompositeViewHoldersForTesting(), holdsNoViews)
      << "a frame that never reached endFrame must release every entry's views";
  renderer.endFrame();
  drawAndExpectRebuilt("mask_and_clipped_after_abandoned_frame");
}

/// A renderer whose frames alternate between documents, as a compositor's layers do, drops the
/// composite entries of each frame that draws none, but keeps their uniform buffers for the next
/// frame that does. Here a mask composite and a layer composited under a path clip that moves every
/// time alternate with a document of neither: after the first round, the composite frames create no
/// buffer or texture, and each matches a fresh renderer.
TEST_F(RendererGeodeTest, RetainedCompositesKeepTheirBuffersAcrossFramesWithoutThem) {
  std::optional<SVGDocument> composites;
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
        <mask id="m"><rect width="64" height="64" fill="#a0a0a0"/></mask>
        <clipPath id="c"><circle id="moving" cx="40" cy="44" r="14"/></clipPath>
        <rect width="32" height="32" fill="orange" mask="url(#m)"/>
        <g clip-path="url(#c)">
          <g opacity="0.5"><rect x="16" y="24" width="48" height="40" fill="red"/></g>
        </g>
      </svg>)svg",
      composites));
  std::optional<SVGDocument> plain;
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
        <rect width="16" height="16" fill="blue"/>
      </svg>)svg",
      plain));
  std::optional<SVGElement> moving = composites->querySelector("#moving");
  ASSERT_THAT(moving, testing::Optional(testing::_)) << "the fixture names this element";

  RendererGeode renderer = createRenderer();
  renderer.draw(*composites);
  ASSERT_THAT(renderer.retainedCompositeCountsForTesting(),
              testing::AllOf(RetainedLayerCount(1), RetainedMaskCount(1)))
      << "Premise: the document draws one mask composite and one layer composite";
  ASSERT_THAT(renderer.layerCompositeTexturesForTesting(), testing::SizeIs(1u));
  renderer.draw(*plain);
  ASSERT_THAT(renderer.retainedCompositeCountsForTesting(),
              testing::AllOf(RetainedLayerCount(0), RetainedMaskCount(0)))
      << "Premise: a frame that draws no composite drops their entries";

  for (int round = 1; round <= 3; ++round) {
    SCOPED_TRACE(testing::Message() << "round " << round);
    moving->setAttribute("cx", std::to_string(36 + 4 * round));
    renderer.draw(*composites);
    const geode::GeodeCounters counters = renderer.lastFrameTimings().counters;
    EXPECT_EQ(counters.bufferCreates, 0u)
        << "the composites reuse the uniform buffers of the entries the plain frame dropped";
    EXPECT_EQ(counters.textureCreates, 0u);
    editor::tests::CompareBitmapToBitmap(
        renderer.takeSnapshot(), RenderFresh(sharedDevice(), *composites),
        "composites_after_a_frame_without_them_" + std::to_string(round),
        editor::tests::PixelmatchIdentityParams());
    renderer.draw(*plain);
  }
}

/// A concurrent-DOM document is drawn by recording a snapshot and replaying it. Mask composites are
/// kept by the renderer, not the document, so a replayed unchanged frame composites each mask from
/// the textures of the frame before, and the replay paints what a direct draw paints.
TEST_F(RendererGeodeTest, SnapshotReplayKeepsMaskCompositeBindings) {
  std::optional<SVGDocument> direct;
  std::optional<SVGDocument> replayed;
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(kMaskCompositeSvg, direct));
  ASSERT_NO_FATAL_FAILURE(ParseDocumentWithoutWarnings(kMaskCompositeSvg, replayed));
  replayed->setThreadingMode(ThreadingMode::ConcurrentDom);

  RendererGeode replayRenderer = createRenderer();
  RendererDriver driver(replayRenderer);
  driver.draw(*replayed);
  const auto firstReplay = replayRenderer.maskCompositeTexturesForTesting();
  ASSERT_THAT(firstReplay, testing::SizeIs(kMaskCompositeCount));
  driver.draw(*replayed);
  EXPECT_THAT(replayRenderer.maskCompositeTexturesForTesting(),
              testing::ElementsAreArray(firstReplay));
  editor::tests::CompareBitmapToBitmap(
      replayRenderer.takeSnapshot(), RenderFresh(sharedDevice(), *direct),
      "mask_composite_snapshot_replay", editor::tests::PixelmatchIdentityParams());
}

/// A circle filtered by \p primitives whose filter region is the whole `0 0 W H` view box, so the
/// filter's capture covers the canvas with no margin, and a canvas reduced by a power of two holds
/// exactly the pixels of a raster reduced by the same factor. With \p backdrop, a gray rectangle
/// lies under the circle.
std::string WholeCanvasFilterSource(std::string_view primitives, int viewBoxWidth = 64,
                                    int viewBoxHeight = 64, bool backdrop = false,
                                    bool filtered = true, std::string_view elementTransform = {}) {
  const std::string width = std::to_string(viewBoxWidth);
  const std::string height = std::to_string(viewBoxHeight);
  std::string source = "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 " + width + " " +
                       height + "\"><filter id=\"f\" filterUnits=\"userSpaceOnUse\" x=\"0\" " +
                       "y=\"0\" width=\"" + width + "\" height=\"" + height + "\">" +
                       std::string(primitives) + "</filter>";
  if (backdrop) {
    source += "<rect x=\"4\" y=\"40\" width=\"56\" height=\"20\" fill=\"#888\"/>";
  }
  if (filtered) {
    source +=
        "<circle cx=\"32\" cy=\"32\" r=\"18\" fill=\"#3a7\" stroke=\"#124\" "
        "stroke-width=\"3\" filter=\"url(#f)\"";
    if (!elementTransform.empty()) {
      source += " transform=\"" + std::string(elementTransform) + "\"";
    }
    source += "/>";
  }
  return source + "</svg>";
}

/// \p source parsed and rendered on a fresh renderer of the shared device at \p canvas, with the
/// frame's surface bytes limited to \p surfaceBytes when given. \p stats receives the frame's
/// resource statistics.
RendererBitmap RenderFilterCanvas(const std::shared_ptr<geode::GeodeDevice>& device,
                                  const std::string& source, Vector2i canvas,
                                  std::optional<std::uint64_t> surfaceBytes,
                                  RendererResourceStats& stats) {
  std::optional<SVGDocument> document;
  ParseDocumentWithoutWarnings(source, document);
  if (!document.has_value()) {
    return {};
  }
  document->setCanvasSize(canvas.x, canvas.y);
  RendererGeode renderer(device);
  if (surfaceBytes.has_value()) {
    renderer.setSurfaceBudgetForTesting(RendererSurfaceBudget::kMaximumSurfaces, *surfaceBytes);
  }
  renderer.draw(*document);
  stats = renderer.resourceStats();
  return renderer.takeSnapshot();
}

/// \p bitmap with every pixel repeated over a \p factor x \p factor block: what a raster reduced
/// by \p factor shows when each device pixel takes the raster pixel covering its center.
RendererBitmap ReplicatePixels(const RendererBitmap& bitmap, int factor) {
  RendererBitmap result;
  result.dimensions = bitmap.dimensions * factor;
  result.rowBytes = static_cast<std::size_t>(result.dimensions.x) * 4;
  result.alphaType = bitmap.alphaType;
  result.pixels.resize(result.rowBytes * static_cast<std::size_t>(result.dimensions.y));
  for (int y = 0; y < result.dimensions.y; ++y) {
    for (int x = 0; x < result.dimensions.x; ++x) {
      const std::size_t from = static_cast<std::size_t>(y / factor) * bitmap.rowBytes +
                               static_cast<std::size_t>(x / factor) * 4;
      const std::size_t to =
          static_cast<std::size_t>(y) * result.rowBytes + static_cast<std::size_t>(x) * 4;
      std::copy_n(bitmap.pixels.begin() + static_cast<std::ptrdiff_t>(from), 4,
                  result.pixels.begin() + static_cast<std::ptrdiff_t>(to));
    }
  }
  return result;
}

/// Matches resource statistics whose filters ran on \p rasters reduced rasters, the coarsest
/// reduced by \p reduction.
testing::Matcher<const RendererResourceStats&> ReducedFilterRasters(std::uint64_t rasters,
                                                                    std::uint32_t reduction) {
  return testing::AllOf(
      testing::Field("filterReducedRasters", &RendererResourceStats::filterReducedRasters, rasters),
      testing::Field("filterRasterReduction", &RendererResourceStats::filterRasterReduction,
                     reduction),
      testing::Field("filterBudgetRejected", &RendererResourceStats::filterBudgetRejected, false));
}

/// Matches resource statistics whose filters ran in tiles in \p graphs graphs.
testing::Matcher<const RendererResourceStats&> TiledFilterGraphs(std::uint64_t graphs) {
  return testing::Field("filterTiledGraphs", &RendererResourceStats::filterTiledGraphs, graphs);
}

struct ReducedFilterCase {
  std::string_view name;
  std::string_view primitives;
};

/// Primitives whose result depends on the device resolution: noise frequency and displacement
/// scale, light positions and surface scale, blur deviation and offset, and morphology radius.
constexpr std::array<ReducedFilterCase, 5> kResolutionDependentFilters = {{
    {"turbulence_displacement",
     R"(<feTurbulence type="turbulence" baseFrequency="0.1" numOctaves="2" result="t"/>
        <feDisplacementMap in="SourceGraphic" in2="t" scale="6" xChannelSelector="R"
                           yChannelSelector="G"/>)"},
    {"spot_lighting",
     R"(<feSpecularLighting surfaceScale="3" specularConstant="1.2" specularExponent="8"
                            lighting-color="white" result="s">
          <feSpotLight x="10" y="10" z="30" pointsAtX="32" pointsAtY="32"
                       limitingConeAngle="30"/>
        </feSpecularLighting>
        <feComposite in="SourceGraphic" in2="s" operator="arithmetic" k1="0" k2="1" k3="1"
                     k4="0"/>)"},
    {"distant_diffuse_lighting",
     R"(<feDiffuseLighting surfaceScale="4" diffuseConstant="1">
          <feDistantLight azimuth="45" elevation="40"/>
        </feDiffuseLighting>)"},
    {"blur_offset", R"(<feGaussianBlur stdDeviation="2.5"/><feOffset dx="3" dy="2"/>)"},
    {"morphology", R"(<feMorphology operator="dilate" radius="2"/>)"},
}};

/// A filter the frame's surface budget cannot hold at full resolution runs on a raster reduced by
/// the smallest power of two that fits, in that raster's pixels: the element shows exactly the
/// canvas reduced by that factor, each pixel covering a block of device pixels. The same filter
/// with room runs at full resolution.
TEST_F(RendererGeodeTest, FilterPastTheSurfaceBudgetRunsOnAReducedRaster) {
  constexpr int kCanvas = 256;
  // Holds the target and a filter at half resolution, but not at full resolution.
  constexpr std::uint64_t kSurfaceBytes = 2560 * 1024;
  for (const ReducedFilterCase& filter : kResolutionDependentFilters) {
    SCOPED_TRACE(filter.name);
    const std::string source = WholeCanvasFilterSource(filter.primitives);
    RendererResourceStats stats;
    const RendererBitmap reference =
        RenderFilterCanvas(sharedDevice(), source, {kCanvas / 2, kCanvas / 2}, std::nullopt, stats);
    ASSERT_THAT(test::CountNonTransparentPixels(reference), testing::Gt(0u))
        << "Premise: the filtered circle is visible";
    (void)RenderFilterCanvas(sharedDevice(), source, {kCanvas, kCanvas}, std::nullopt, stats);
    EXPECT_THAT(stats, ReducedFilterRasters(0, 1))
        << "With room the filter runs at full resolution";

    const RendererBitmap reduced =
        RenderFilterCanvas(sharedDevice(), source, {kCanvas, kCanvas}, kSurfaceBytes, stats);
    EXPECT_THAT(stats, ReducedFilterRasters(1, 2));
    editor::tests::CompareBitmapToBitmap(reduced, ReplicatePixels(reference, 2),
                                         "reduced_filter_raster_" + std::string(filter.name),
                                         editor::tests::PixelmatchIdentityParams());
  }
}

/// The reduced-raster statistics describe one frame: a later frame whose filters all fit reports
/// none.
TEST_F(RendererGeodeTest, ReducedRasterStatisticsDescribeOneFrame) {
  std::optional<SVGDocument> reduced;
  ParseDocumentWithoutWarnings(WholeCanvasFilterSource(kResolutionDependentFilters[0].primitives),
                               reduced);
  std::optional<SVGDocument> unfiltered;
  ParseDocumentWithoutWarnings(
      WholeCanvasFilterSource(kResolutionDependentFilters[0].primitives, 64, 64,
                              /*backdrop=*/true, /*filtered=*/false),
      unfiltered);
  ASSERT_THAT(reduced, testing::Optional(testing::_));
  ASSERT_THAT(unfiltered, testing::Optional(testing::_));
  reduced->setCanvasSize(256, 256);
  unfiltered->setCanvasSize(256, 256);
  RendererGeode renderer = createRenderer();
  renderer.setSurfaceBudgetForTesting(RendererSurfaceBudget::kMaximumSurfaces, 2560 * 1024);
  renderer.draw(*reduced);
  ASSERT_THAT(renderer.resourceStats(), ReducedFilterRasters(1, 2));
  renderer.draw(*unfiltered);
  EXPECT_THAT(renderer.resourceStats(), ReducedFilterRasters(0, 1));
}

/// Popping a filter that ran on a reduced raster restores the transform in effect before it was
/// pushed, so a caller that draws next without setting a transform draws in device pixels.
TEST_F(RendererGeodeTest, DrawAfterAReducedFilterUsesTheTransformBeforeIt) {
  constexpr double kSize = 256.0;
  const auto render = [&](bool setTransformAfterPop, RendererResourceStats& stats) {
    RendererGeode renderer = createRenderer();
    // Holds the target and a full-region flood at half resolution, but not at full resolution.
    renderer.setSurfaceBudgetForTesting(RendererSurfaceBudget::kMaximumSurfaces,
                                        256 * 256 * 4 + 2 * 1024 * 1024);
    RenderViewport viewport;
    viewport.size = Vector2d(kSize, kSize);
    renderer.beginFrame(viewport);
    renderer.setTransform(Transform2d());
    components::FilterGraph graph;
    components::FilterNode flood;
    flood.primitive =
        components::filter_primitive::Flood{.floodColor = css::Color(css::RGBA(0, 0, 255, 255))};
    graph.nodes.push_back(flood);
    renderer.pushFilterLayer(graph, Box2d({0, 0}, {kSize, kSize}));
    renderer.setPaint(solidFill(css::RGBA(255, 255, 255, 255)));
    renderer.drawRect(Box2d({0, 0}, {kSize, kSize}), StrokeParams{});
    renderer.popFilterLayer();
    stats = renderer.resourceStats();
    if (setTransformAfterPop) {
      renderer.setTransform(Transform2d());
    }
    renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
    renderer.drawRect(Box2d({128, 128}, {192, 192}), StrokeParams{});
    renderer.endFrame();
    return renderer.takeSnapshot();
  };
  RendererResourceStats inherited;
  RendererResourceStats explicitTransform;
  const RendererBitmap drawnAfterPop = render(/*setTransformAfterPop=*/false, inherited);
  const RendererBitmap reference = render(/*setTransformAfterPop=*/true, explicitTransform);
  ASSERT_THAT(inherited, ReducedFilterRasters(1, 2));
  ASSERT_THAT(explicitTransform, ReducedFilterRasters(1, 2));
  editor::tests::CompareBitmapToBitmap(drawnAfterPop, reference, "draw_after_reduced_filter",
                                       editor::tests::PixelmatchIdentityParams());
}

/// The work estimate charges feMorphology its largest radius, so on a 1024 x 1024 canvas its work
/// is past what one graph may do; it runs on a raster reduced by four, the smallest power of two
/// that brings the work under the limit, and shows the 256 x 256 rendering.
TEST_F(RendererGeodeTest, FilterPastThePerGraphWorkLimitRunsOnAReducedRaster) {
  const std::string source = WholeCanvasFilterSource(kResolutionDependentFilters[4].primitives);
  RendererResourceStats stats;
  const RendererBitmap reference =
      RenderFilterCanvas(sharedDevice(), source, {256, 256}, std::nullopt, stats);
  ASSERT_THAT(stats, ReducedFilterRasters(0, 1));
  ASSERT_THAT(test::CountNonTransparentPixels(reference), testing::Gt(0u));
  const RendererBitmap large =
      RenderFilterCanvas(sharedDevice(), source, {1024, 1024}, std::nullopt, stats);
  EXPECT_THAT(stats, ReducedFilterRasters(1, 4));
  editor::tests::CompareBitmapToBitmap(large, ReplicatePixels(reference, 4),
                                       "morphology_past_work_limit",
                                       editor::tests::PixelmatchIdentityParams());
}

/// The frame's work limit covers every filter it runs. Each feMorphology over a 256 x 256 canvas
/// is charged its largest radius, so the frame's work runs out after seven of them; the later ones
/// run on a raster reduced by two instead of being dropped.
TEST_F(RendererGeodeTest, FiltersPastTheFrameWorkLimitRunOnReducedRasters) {
  constexpr int kFilters = 10;
  std::string source =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 64 64\"><filter id=\"f\" "
      "filterUnits=\"userSpaceOnUse\" x=\"0\" y=\"0\" width=\"64\" height=\"64\">"
      "<feMorphology operator=\"dilate\" radius=\"1\"/></filter>";
  for (int i = 0; i < kFilters; ++i) {
    source +=
        "<circle cx=\"" + std::to_string(4 + 6 * i) + "\" cy=\"32\" r=\"2\" filter=\"url(#f)\"/>";
  }
  source += "</svg>";
  RendererResourceStats stats;
  (void)RenderFilterCanvas(sharedDevice(), source, {256, 256}, std::nullopt, stats);
  EXPECT_THAT(
      stats,
      testing::AllOf(testing::Field("filterExecutions", &RendererResourceStats::filterExecutions,
                                    static_cast<std::uint64_t>(kFilters)),
                     testing::Field("filterReducedRasters",
                                    &RendererResourceStats::filterReducedRasters, testing::Gt(0u)),
                     testing::Field("filterRasterReduction",
                                    &RendererResourceStats::filterRasterReduction, 2u),
                     testing::Field("filterBudgetRejected",
                                    &RendererResourceStats::filterBudgetRejected, false)));
}

/// A filter that does not fit even a raster reduced by 16 is left out, as a refused filter was
/// before: the content under it still draws, and the frame reports the refusal.
TEST_F(RendererGeodeTest, FilterPastTheCoarsestReducedRasterIsLeftOut) {
  // Large enough that the filter's intermediates, not its fixed parameter blocks, decide the fit.
  constexpr int kCanvas = 2048;
  // Holds the target and the filter on a raster reduced by 32 (64 x 64), not by 16 (128 x 128).
  constexpr std::uint64_t kSurfaceBytes = std::uint64_t{kCanvas} * kCanvas * 4 + 1280 * 1024;
  const std::string_view primitives = kResolutionDependentFilters[0].primitives;
  RendererResourceStats stats;
  const RendererBitmap leftOut = RenderFilterCanvas(
      sharedDevice(), WholeCanvasFilterSource(primitives, 64, 64, /*backdrop=*/true),
      {kCanvas, kCanvas}, kSurfaceBytes, stats);
  EXPECT_THAT(stats,
              testing::AllOf(testing::Field("filterReducedRasters",
                                            &RendererResourceStats::filterReducedRasters, 0u),
                             testing::Field("filterBudgetRejected",
                                            &RendererResourceStats::filterBudgetRejected, true)));
  RendererResourceStats referenceStats;
  const RendererBitmap withoutElement = RenderFilterCanvas(
      sharedDevice(),
      WholeCanvasFilterSource(primitives, 64, 64, /*backdrop=*/true, /*filtered=*/false),
      {kCanvas, kCanvas}, std::nullopt, referenceStats);
  ASSERT_THAT(test::CountNonTransparentPixels(withoutElement), testing::Gt(0u))
      << "Premise: the backdrop is visible";
  editor::tests::CompareBitmapToBitmap(leftOut, withoutElement, "filter_past_coarsest_raster",
                                       editor::tests::PixelmatchIdentityParams());
}

/// A turbulence and displacement filter on a rotated element over a 2048 x 2048 canvas needs more
/// intermediate memory than one graph may hold, and a rotated filter cannot split into tiles, so it
/// runs on a raster reduced by two: the canvas shows the 1024 x 1024 rendering, each pixel covering
/// a 2 x 2 block, rather than nothing.
TEST_F(RendererGeodeTest, FilterOnA2048CanvasRunsOnAReducedRaster) {
  const std::string source = WholeCanvasFilterSource(kResolutionDependentFilters[0].primitives, 64,
                                                     64, false, true, "rotate(30 32 32)");
  RendererResourceStats stats;
  const RendererBitmap reference =
      RenderFilterCanvas(sharedDevice(), source, {1024, 1024}, std::nullopt, stats);
  ASSERT_THAT(stats, ReducedFilterRasters(0, 1));
  ASSERT_THAT(test::CountNonTransparentPixels(reference), testing::Gt(0u));
  const RendererBitmap large =
      RenderFilterCanvas(sharedDevice(), source, {2048, 2048}, std::nullopt, stats);
  EXPECT_THAT(stats, ReducedFilterRasters(1, 2));
  editor::tests::CompareBitmapToBitmap(large, ReplicatePixels(reference, 2),
                                       "filter_on_2048_canvas",
                                       editor::tests::PixelmatchIdentityParams());
}

/// On a 3840 x 2160 canvas the same rotated filter also exceeds the frame's surface budget at half
/// resolution, so it runs on a raster reduced by four.
TEST_F(RendererGeodeTest, FilterOnA3840x2160CanvasRunsOnAReducedRaster) {
  const std::string source = WholeCanvasFilterSource(kResolutionDependentFilters[0].primitives, 128,
                                                     72, false, true, "rotate(30 64 36)");
  RendererResourceStats stats;
  const RendererBitmap reference =
      RenderFilterCanvas(sharedDevice(), source, {960, 540}, std::nullopt, stats);
  ASSERT_THAT(stats, ReducedFilterRasters(0, 1));
  ASSERT_THAT(test::CountNonTransparentPixels(reference), testing::Gt(0u));
  const RendererBitmap large =
      RenderFilterCanvas(sharedDevice(), source, {3840, 2160}, std::nullopt, stats);
  EXPECT_THAT(stats, ReducedFilterRasters(1, 4));
  editor::tests::CompareBitmapToBitmap(large, ReplicatePixels(reference, 4),
                                       "filter_on_3840x2160_canvas",
                                       editor::tests::PixelmatchIdentityParams());
}

/// Filters a reduced raster cannot reproduce: a skewed blur and a rotated one-axis blur, which the
/// reduced canvas draws on the transformed blur path; a backdrop read; and a pre-rasterized feImage
/// fragment. Past the injected budget each runs at full resolution if it fits there, and is left
/// out otherwise, but never on a reduced raster.
TEST_F(RendererGeodeTest, FiltersAReducedRasterCannotReproduceAreNotReduced) {
  constexpr std::uint64_t kSurfaceBytes = 2560 * 1024;
  struct Case {
    std::string_view name;
    std::string body;
  };
  const std::string defs =
      "<filter id=\"f\" filterUnits=\"userSpaceOnUse\" x=\"0\" y=\"0\" width=\"64\" height=\"64\">";
  const std::array<Case, 4> cases = {{
      {"skewed_blur",
       defs + "<feGaussianBlur stdDeviation=\"2\"/></filter><g transform=\"skewX(30)\">"},
      {"rotated_one_axis_blur",
       defs + "<feGaussianBlur stdDeviation=\"3 0\"/></filter><g transform=\"rotate(30 32 32)\">"},
      {"backdrop_blend", defs + "<feBlend in=\"SourceGraphic\" in2=\"BackgroundImage\" "
                                "mode=\"multiply\"/></filter><g>"},
      {"fragment_image",
       "<defs><rect id=\"frag\" x=\"8\" y=\"8\" width=\"30\" height=\"20\" fill=\"#a35\"/></defs>" +
           defs + "<feImage href=\"#frag\"/></filter><g>"},
  }};
  for (const Case& test : cases) {
    SCOPED_TRACE(test.name);
    const auto source = [&](bool withElement) {
      return "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 64 64\">"
             "<rect x=\"4\" y=\"40\" width=\"56\" height=\"20\" fill=\"#888\"/>" +
             test.body +
             (withElement ? "<circle cx=\"32\" cy=\"32\" r=\"18\" fill=\"#3a7\" "
                            "filter=\"url(#f)\"/>"
                          : "") +
             "</g></svg>";
    };
    RendererResourceStats stats;
    const RendererBitmap budgeted =
        RenderFilterCanvas(sharedDevice(), source(true), {256, 256}, kSurfaceBytes, stats);
    EXPECT_THAT(stats, testing::Field("filterReducedRasters",
                                      &RendererResourceStats::filterReducedRasters, 0u));
    RendererResourceStats referenceStats;
    const RendererBitmap reference =
        stats.filterBudgetRejected ? RenderFilterCanvas(sharedDevice(), source(false), {256, 256},
                                                        std::nullopt, referenceStats)
                                   : RenderFilterCanvas(sharedDevice(), source(true), {256, 256},
                                                        std::nullopt, referenceStats);
    editor::tests::CompareBitmapToBitmap(budgeted, reference,
                                         "not_reduced_" + std::string(test.name),
                                         editor::tests::PixelmatchIdentityParams());
  }
}

/// A filter region inset from the canvas at offsets that are not multiples of the reduction runs
/// on a raster whose pixels are the reduced canvas's, so it matches the half-size rendering
/// doubled.
TEST_F(RendererGeodeTest, InsetFilterRegionRunsOnTheReducedCanvasGrid) {
  constexpr std::uint64_t kSurfaceBytes = 2560 * 1024;
  for (const std::size_t index : {std::size_t{0}, std::size_t{3}}) {
    const ReducedFilterCase& filter = kResolutionDependentFilters[index];
    SCOPED_TRACE(filter.name);
    std::string source = WholeCanvasFilterSource(filter.primitives);
    const std::string region = "x=\"0\" y=\"0\" width=\"64\" height=\"64\"";
    source.replace(source.find(region), region.size(),
                   "x=\"3.3\" y=\"5.7\" width=\"47\" height=\"41\"");
    RendererResourceStats stats;
    const RendererBitmap reference =
        RenderFilterCanvas(sharedDevice(), source, {128, 128}, std::nullopt, stats);
    ASSERT_THAT(test::CountNonTransparentPixels(reference), testing::Gt(0u));
    const RendererBitmap reduced =
        RenderFilterCanvas(sharedDevice(), source, {256, 256}, kSurfaceBytes, stats);
    EXPECT_THAT(stats, ReducedFilterRasters(1, 2));
    editor::tests::CompareBitmapToBitmap(reduced, ReplicatePixels(reference, 2),
                                         "inset_reduced_" + std::string(filter.name),
                                         editor::tests::PixelmatchIdentityParams());
  }
}

/// The unchanged second frame of a filter on a reduced raster costs what the same filter's second
/// frame costs at full resolution: nothing is encoded, created or rebuilt again.
TEST_F(RendererGeodeTest, ReducedFilterReachesTheSteadyState) {
  const auto secondFrame = [&](std::optional<std::uint64_t> surfaceBytes,
                               RendererResourceStats& stats) {
    std::optional<SVGDocument> document;
    ParseDocumentWithoutWarnings(WholeCanvasFilterSource(kResolutionDependentFilters[3].primitives),
                                 document);
    document->setCanvasSize(256, 256);
    RendererGeode renderer = createRenderer();
    if (surfaceBytes.has_value()) {
      renderer.setSurfaceBudgetForTesting(RendererSurfaceBudget::kMaximumSurfaces, *surfaceBytes);
    }
    renderer.draw(*document);
    (void)renderer.takeSnapshot();
    renderer.draw(*document);
    stats = renderer.resourceStats();
    return renderer.lastFrameTimings().counters;
  };
  RendererResourceStats fullStats;
  RendererResourceStats reducedStats;
  const geode::GeodeCounters full = secondFrame(std::nullopt, fullStats);
  const geode::GeodeCounters reduced = secondFrame(2560 * 1024, reducedStats);
  ASSERT_THAT(fullStats, ReducedFilterRasters(0, 1));
  ASSERT_THAT(reducedStats, ReducedFilterRasters(1, 2));
  EXPECT_THAT(
      reduced,
      testing::AllOf(
          testing::Field("pathEncodes", &geode::GeodeCounters::pathEncodes, full.pathEncodes),
          testing::Field("textureCreates", &geode::GeodeCounters::textureCreates,
                         full.textureCreates),
          testing::Field("bufferWrites", &geode::GeodeCounters::bufferWrites, full.bufferWrites),
          testing::Field("bindgroupCreates", &geode::GeodeCounters::bindgroupCreates,
                         full.bindgroupCreates)));
}

/// A filter too large for the whole surface budget runs on a reduced raster without first
/// splitting the frame, because no submitted chunk could make room for it.
TEST_F(RendererGeodeTest, FilterLargerThanTheSurfaceBudgetReducesWithoutAChunk) {
  constexpr int kCanvas = 256;
  RendererGeode renderer = createRenderer();
  renderer.setSurfaceBudgetForTesting(RendererSurfaceBudget::kMaximumSurfaces, 2560 * 1024);
  RenderViewport viewport;
  viewport.size = Vector2d(kCanvas, kCanvas);
  renderer.beginFrame(viewport);
  renderer.setTransform(Transform2d());
  // A small first filter leaves retained filter memory, so a chunk would be available.
  components::FilterGraph flood;
  components::FilterNode floodNode;
  floodNode.primitive =
      components::filter_primitive::Flood{.floodColor = css::Color(css::RGBA(0, 0, 255, 255))};
  flood.nodes.push_back(floodNode);
  renderer.pushFilterLayer(flood, Box2d({0, 0}, {16, 16}));
  renderer.popFilterLayer();
  components::FilterGraph blur;
  components::FilterNode blurNode;
  blurNode.primitive =
      components::filter_primitive::GaussianBlur{.stdDeviationX = 3, .stdDeviationY = 3};
  blur.nodes.push_back(blurNode);
  renderer.pushFilterLayer(blur, Box2d({0, 0}, {kCanvas, kCanvas}));
  renderer.setPaint(solidFill(css::RGBA(255, 255, 255, 255)));
  renderer.drawRect(Box2d({32, 32}, {224, 224}), StrokeParams{});
  renderer.popFilterLayer();
  const RendererResourceStats stats = renderer.resourceStats();
  renderer.endFrame();
  EXPECT_THAT(stats, ReducedFilterRasters(1, 2));
  EXPECT_THAT(renderer.filterBudgetChunksForTesting(), testing::Eq(0u));
}

/// Filters whose sampling halo is known once each tile carries its origin in the raster: a
/// displacement map, every lighting primitive under every kind of light, both noise types with and
/// without stitching, and the combinations the corpus and authors use.
constexpr std::array<ReducedFilterCase, 20> kTileableFilters = {{
    {"displacement",
     R"(<feDisplacementMap in="SourceGraphic" in2="SourceGraphic" scale="9" xChannelSelector="R"
                           yChannelSelector="G"/>)"},
    {"diffuse_distant", kResolutionDependentFilters[2].primitives},
    {"diffuse_point",
     R"(<feDiffuseLighting surfaceScale="3" diffuseConstant="1.3" lighting-color="#adf">
          <fePointLight x="13.7" y="51.3" z="17.9"/>
        </feDiffuseLighting>)"},
    {"diffuse_spot",
     R"(<feDiffuseLighting surfaceScale="2.5" diffuseConstant="1">
          <feSpotLight x="57.3" y="6.1" z="29.7" pointsAtX="30.2" pointsAtY="35.9"
                       specularExponent="3" limitingConeAngle="35"/>
        </feDiffuseLighting>)"},
    {"specular_distant",
     R"(<feSpecularLighting surfaceScale="3" specularConstant="1.1" specularExponent="9">
          <feDistantLight azimuth="133" elevation="27"/>
        </feSpecularLighting>)"},
    {"specular_point",
     R"(<feSpecularLighting surfaceScale="2" specularConstant="1" specularExponent="12"
                            lighting-color="#fd8">
          <fePointLight x="50.3" y="12.7" z="20.1"/>
        </feSpecularLighting>)"},
    {"specular_spot", kResolutionDependentFilters[1].primitives},
    {"turbulence", R"(<feTurbulence type="turbulence" baseFrequency="0.07" numOctaves="3"/>)"},
    {"turbulence_stitched",
     R"(<feTurbulence type="turbulence" baseFrequency="0.09" numOctaves="2" stitchTiles="stitch"/>)"},
    {"fractal_noise",
     R"(<feTurbulence type="fractalNoise" baseFrequency="0.05 0.11" numOctaves="2" seed="7"/>)"},
    {"fractal_noise_stitched",
     R"(<feTurbulence type="fractalNoise" baseFrequency="0.06" numOctaves="3"
                      stitchTiles="stitch"/>)"},
    {"turbulence_into_displacement",
     R"(<feTurbulence type="turbulence" baseFrequency="0.1" numOctaves="2" result="t"/>
        <feDisplacementMap in="SourceGraphic" in2="t" scale="2.3" xChannelSelector="R"
                           yChannelSelector="G"/>)"},
    {"blur_into_lighting",
     R"(<feGaussianBlur in="SourceAlpha" stdDeviation="1.3" result="b"/>
        <feSpecularLighting in="b" surfaceScale="4" specularConstant="1" specularExponent="7">
          <feSpotLight x="9.3" y="55.1" z="25.3" pointsAtX="33" pointsAtY="30"
                       limitingConeAngle="40"/>
        </feSpecularLighting>
        <feComposite in2="SourceGraphic" operator="in"/>)"},
    {"blur_into_displacement",
     R"(<feGaussianBlur stdDeviation="1.5" result="b"/>
        <feDisplacementMap in="b" in2="SourceGraphic" scale="-3.7" xChannelSelector="G"
                           yChannelSelector="A"/>)"},
    {"turbulence_into_lighting",
     R"(<feTurbulence type="fractalNoise" baseFrequency="0.08" numOctaves="2" result="t"/>
        <feDiffuseLighting in="t" surfaceScale="5" diffuseConstant="1.1">
          <fePointLight x="21.7" y="44.9" z="12.3"/>
        </feDiffuseLighting>)"},
    {"lighting_into_displacement",
     R"(<feSpecularLighting in="SourceAlpha" surfaceScale="4" specularConstant="1"
                            specularExponent="5" result="l">
          <feSpotLight x="60.1" y="3.7" z="22.9" pointsAtX="20.3" pointsAtY="41.1"/>
        </feSpecularLighting>
        <feDisplacementMap in="SourceGraphic" in2="l" scale="7.9" xChannelSelector="R"
                           yChannelSelector="B"/>)"},
    {"lighting_subregions",
     R"(<feGaussianBlur in="SourceAlpha" stdDeviation="0.9" x="7.3" y="5.1" width="40.7"
                       height="47.9" result="b"/>
        <feDiffuseLighting in="b" surfaceScale="4" diffuseConstant="1.2" x="17.3" y="9.1"
                           width="30.7" height="41.9">
          <fePointLight x="21.7" y="44.9" z="12.3"/>
        </feDiffuseLighting>)"},
    {"specular_spot_subregion",
     R"(<feSpecularLighting in="SourceAlpha" surfaceScale="3" specularConstant="1"
                            specularExponent="6" x="11.9" y="13.3" width="37.1" height="29.7">
          <feSpotLight x="50.3" y="7.7" z="23.1" pointsAtX="30.1" pointsAtY="33.3"
                       limitingConeAngle="33"/>
        </feSpecularLighting>)"},
    {"displacement_by_noise_subregion",
     R"(<feTurbulence type="fractalNoise" baseFrequency="0.13" numOctaves="2" x="9.7" y="12.1"
                      width="33.3" height="30.9" result="t"/>
        <feDisplacementMap in="SourceGraphic" in2="t" scale="6.3" xChannelSelector="A"
                           yChannelSelector="B" x="5.5" y="3.3" width="51.1" height="49.7"/>)"},
    {"displacement_chain",
     R"(<feGaussianBlur in="SourceGraphic" stdDeviation="1.1" result="b"/>
        <feDisplacementMap in="SourceGraphic" in2="b" scale="5.9" xChannelSelector="G"
                           yChannelSelector="R" result="d1"/>
        <feDisplacementMap in="d1" in2="d1" scale="-4.3" xChannelSelector="B"
                           yChannelSelector="G"/>)"},
}};

/// \p source rendered at \p canvas on a fresh device whose filter engine executes at most
/// \p maximumTileExtent pixels per tile edge, or chooses its own tiles when zero. \p tiles receives
/// the number of tiles the last filter ran in.
RendererBitmap RenderOnTiledDevice(const std::string& source, Vector2i canvas,
                                   uint32_t maximumTileExtent, uint64_t& tiles,
                                   RendererResourceStats& stats) {
  const std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
  EXPECT_THAT(device, testing::NotNull());
  std::optional<SVGDocument> document;
  ParseDocumentWithoutWarnings(source, document);
  if (!device || !document) {
    return {};
  }
  if (maximumTileExtent != 0) {
    device->filterEngine().setMaximumTileExtentForTesting(maximumTileExtent);
  }
  document->setCanvasSize(canvas.x, canvas.y);
  RendererGeode renderer(device);
  renderer.draw(*document);
  tiles = device->filterEngine().lastExecutionMemory().tileExecutions;
  stats = renderer.resourceStats();
  return renderer.takeSnapshot();
}

class TiledFilterExactnessTest : public RendererGeodeTest,
                                 public testing::WithParamInterface<ReducedFilterCase> {};

/// Tiles reproduce the untiled raster exactly at several tile sizes, under a uniform and a
/// non-uniform scale. A 900 x 900 canvas and tile extents of 512, 487 and 333 put tile origins at
/// positions that are not powers of two, where positions computed from a tile-local pixel and a
/// shifted light or displacement would round differently from the raster's; each tile instead
/// computes every position in the raster's pixels, from its origin.
TEST_P(TiledFilterExactnessTest, TilesMatchTheUntiledRaster) {
  for (const std::string_view transform :
       {std::string_view{}, std::string_view{"scale(1.25 0.75)"}}) {
    SCOPED_TRACE(testing::Message() << "element transform '" << transform << "'");
    const std::string source =
        WholeCanvasFilterSource(GetParam().primitives, 64, 64,
                                /*backdrop=*/false, /*filtered=*/true, transform);
    uint64_t untiledTiles = 0;
    RendererResourceStats stats;
    const RendererBitmap untiled =
        RenderOnTiledDevice(source, {900, 900}, /*maximumTileExtent=*/0, untiledTiles, stats);
    ASSERT_THAT(untiledTiles, testing::Eq(1u));
    ASSERT_THAT(test::CountNonTransparentPixels(untiled), testing::Gt(0u));
    for (const uint32_t extent : {512u, 487u, 333u}) {
      SCOPED_TRACE(testing::Message() << "tile extent " << extent);
      uint64_t tiles = 0;
      const RendererBitmap tiled = RenderOnTiledDevice(source, {900, 900}, extent, tiles, stats);
      ASSERT_THAT(tiles, testing::Gt(1u)) << "Premise: the filter must execute in tiles";
      EXPECT_THAT(stats, TiledFilterGraphs(1));
      editor::tests::CompareBitmapToBitmap(tiled, untiled,
                                           "tiled_" + std::string(GetParam().name) +
                                               (transform.empty() ? "" : "_scaled") + "_" +
                                               std::to_string(extent),
                                           editor::tests::PixelmatchIdentityParams());
    }
  }
}

INSTANTIATE_TEST_SUITE_P(Filters, TiledFilterExactnessTest, testing::ValuesIn(kTileableFilters),
                         [](const testing::TestParamInfo<ReducedFilterCase>& info) {
                           return std::string(info.param.name);
                         });

/// A graph the frame's surface budget cannot hold untiled, even in an empty frame, tries tiles at
/// full resolution before a reduced raster: under a 24 MiB budget a 768 x 768 displacement,
/// lighting or noise graph runs in tiles, never reduced, and equals its untiled rendering.
TEST_F(RendererGeodeTest, GraphPastTheSurfaceBudgetTriesTilesBeforeAReducedRaster) {
  for (const std::size_t index : {std::size_t{0}, std::size_t{1}, std::size_t{7}}) {
    const ReducedFilterCase& filter = kTileableFilters[index];
    SCOPED_TRACE(filter.name);
    const std::string source = WholeCanvasFilterSource(filter.primitives);
    RendererResourceStats stats;
    const RendererBitmap untiled =
        RenderFilterCanvas(sharedDevice(), source, {768, 768}, std::nullopt, stats);
    ASSERT_THAT(stats, TiledFilterGraphs(0)) << "Premise: with room the graph runs untiled";
    const RendererBitmap tiled =
        RenderFilterCanvas(sharedDevice(), source, {768, 768}, 24 * 1024 * 1024, stats);
    EXPECT_THAT(stats, TiledFilterGraphs(1));
    EXPECT_THAT(stats, ReducedFilterRasters(0, 1));
    editor::tests::CompareBitmapToBitmap(tiled, untiled,
                                         "tiled_past_surface_budget_" + std::string(filter.name),
                                         editor::tests::PixelmatchIdentityParams());
  }
}

/// \p source rendered at 900 x 900 matches its untiled raster in tiles at extents 487 and 333.
void ExpectTilesMatchTheUntiledRaster(const std::string& source, std::string_view name) {
  uint64_t untiledTiles = 0;
  RendererResourceStats stats;
  const RendererBitmap untiled =
      RenderOnTiledDevice(source, {900, 900}, /*maximumTileExtent=*/0, untiledTiles, stats);
  ASSERT_THAT(untiledTiles, testing::Eq(1u));
  ASSERT_THAT(test::CountNonTransparentPixels(untiled), testing::Gt(0u));
  for (const uint32_t extent : {487u, 333u}) {
    SCOPED_TRACE(testing::Message() << "tile extent " << extent);
    uint64_t tiles = 0;
    const RendererBitmap tiled = RenderOnTiledDevice(source, {900, 900}, extent, tiles, stats);
    ASSERT_THAT(tiles, testing::Gt(1u)) << "Premise: the filter must execute in tiles";
    EXPECT_THAT(stats, TiledFilterGraphs(1));
    editor::tests::CompareBitmapToBitmap(tiled, untiled,
                                         std::string(name) + "_" + std::to_string(extent),
                                         editor::tests::PixelmatchIdentityParams());
  }
}

/// Tiling admits a transform whose off-diagonal terms are below 1e-6, but the lighting programs
/// treat axes whose squared cosine passes 0.0003 as sheared and place a spot light's cone from the
/// user-space position of each pixel. At about 1e-4 device pixels per unit, `matrix(1 0.009 0.009
/// 1 0 0)` is both, so each tile must compute that position from its origin too.
TEST_F(RendererGeodeTest, ShearedSpotLightTilesMatchTheUntiledRaster) {
  ExpectTilesMatchTheUntiledRaster(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 9000000 9000000">
        <filter id="f" filterUnits="userSpaceOnUse" x="0" y="0" width="9000000" height="9000000">
          <feDiffuseLighting surfaceScale="3" diffuseConstant="1">
            <feSpotLight x="7000000" y="1500000" z="2500000" pointsAtX="4000000"
                         pointsAtY="5000000" limitingConeAngle="35"/>
          </feDiffuseLighting>
        </filter>
        <g transform="matrix(1 0.009 0.009 1 0 0)">
          <circle cx="4400000" cy="4400000" r="3200000" fill="#3a7" filter="url(#f)"/>
        </g>
      </svg>)svg",
      "sheared_spot_light");
}

/// A displacement map in objectBoundingBox primitive units scales by the element's bounding box,
/// and its halo must scale the same way.
TEST_F(RendererGeodeTest, ObjectBoundingBoxDisplacementTilesMatchTheUntiledRaster) {
  std::string source = WholeCanvasFilterSource(
      R"(<feDisplacementMap in="SourceGraphic" in2="SourceGraphic" scale="0.17"
                           xChannelSelector="R" yChannelSelector="A"/>)");
  const std::string filter = "<filter id=\"f\" ";
  source.replace(source.find(filter), filter.size(),
                 "<filter id=\"f\" primitiveUnits=\"objectBoundingBox\" ");
  ExpectTilesMatchTheUntiledRaster(source, "object_bounding_box_displacement");
}

/// Stitched noise wraps its lattice at the raster's size, not a tile's. Below one device pixel per
/// user unit the noise coordinates pass the raster's width, so the wrap shows: tiles 487 and 333
/// pixels wide must wrap where the untiled raster does.
TEST_F(RendererGeodeTest, StitchedNoiseWrapsAtTheRasterSizeInTiles) {
  for (const std::size_t index : {std::size_t{8}, std::size_t{10}}) {
    const ReducedFilterCase& filter = kTileableFilters[index];
    SCOPED_TRACE(filter.name);
    const std::string source = WholeCanvasFilterSource(filter.primitives, 2000, 2000);
    uint64_t untiledTiles = 0;
    RendererResourceStats stats;
    const RendererBitmap untiled =
        RenderOnTiledDevice(source, {900, 900}, /*maximumTileExtent=*/0, untiledTiles, stats);
    ASSERT_THAT(untiledTiles, testing::Eq(1u));
    for (const uint32_t extent : {487u, 333u}) {
      SCOPED_TRACE(testing::Message() << "tile extent " << extent);
      uint64_t tiles = 0;
      const RendererBitmap tiled = RenderOnTiledDevice(source, {900, 900}, extent, tiles, stats);
      ASSERT_THAT(tiles, testing::Gt(1u));
      EXPECT_THAT(stats, TiledFilterGraphs(1));
      editor::tests::CompareBitmapToBitmap(
          tiled, untiled, "stitched_" + std::string(filter.name) + "_" + std::to_string(extent),
          editor::tests::PixelmatchIdentityParams());
    }
  }
}

/// A filter that fits untiled stays untiled, so its pixels are the ones it had before such graphs
/// could tile.
TEST_F(RendererGeodeTest, FilterThatFitsUntiledStaysUntiled) {
  for (const ReducedFilterCase& filter : kTileableFilters) {
    SCOPED_TRACE(filter.name);
    uint64_t tiles = 0;
    RendererResourceStats stats;
    (void)RenderOnTiledDevice(WholeCanvasFilterSource(filter.primitives), {1024, 1024},
                              /*maximumTileExtent=*/0, tiles, stats);
    EXPECT_THAT(tiles, testing::Eq(1u));
    EXPECT_THAT(stats, ReducedFilterRasters(0, 1));
  }
}

/// A large canvas and the view box that fills it. The filter region is the whole view box, and the
/// view box is centered on the filtered circle at (32, 32).
struct LargeCanvas {
  std::string_view name;
  Vector2i canvas;
  double viewBoxX;
  double viewBoxY;
  int viewBoxWidth;
  int viewBoxHeight;
};

/// 2048 x 2048 maps a user unit to 32 device pixels and 3840 x 2160 to 16: powers of two, with view
/// box origins that are short binary fractions.
constexpr std::array<LargeCanvas, 2> kLargeCanvases = {{
    {"2048x2048", {2048, 2048}, 0, 0, 64, 64},
    {"3840x2160", {3840, 2160}, -88, -35.5, 240, 135},
}};

/// `x y w h` of \p viewBox, printed exactly.
std::string ViewBoxText(double x, double y, double width, double height) {
  std::ostringstream text;
  text.precision(17);
  text << x << " " << y << " " << width << " " << height;
  return text.str();
}

/// \p primitives on the circle of `WholeCanvasFilterSource`, filling \p large: its view box and
/// filter region both start at the large canvas's view box origin.
std::string LargeCanvasSource(std::string_view primitives, const LargeCanvas& large) {
  std::string source = WholeCanvasFilterSource(primitives, large.viewBoxWidth, large.viewBoxHeight);
  const std::string viewBox =
      "viewBox=\"" + ViewBoxText(0, 0, large.viewBoxWidth, large.viewBoxHeight) + "\"";
  source.replace(
      source.find(viewBox), viewBox.size(),
      "viewBox=\"" +
          ViewBoxText(large.viewBoxX, large.viewBoxY, large.viewBoxWidth, large.viewBoxHeight) +
          "\"");
  const std::string origin = "x=\"0\" y=\"0\"";
  std::ostringstream moved;
  moved.precision(17);
  moved << "x=\"" << large.viewBoxX << "\" y=\"" << large.viewBoxY << "\"";
  source.replace(source.find(origin), origin.size(), moved.str());
  return source;
}

/// Graphs past the untiled limits on both large canvases (each lighting graph composites its light
/// over the source, a second intermediate) whose position arithmetic is exact in `f32` on any
/// raster: every light position, displacement and pixel position is a short binary
/// fraction, so a quadrant raster starting at its own corner computes the same values as the
/// whole canvas. The displacement map passes through a discrete transfer table, so each
/// displacement is a multiple of a quarter of the scale.
constexpr std::array<ReducedFilterCase, 4> kExactLargeCanvasFilters = {{
    {"displacement",
     R"(<feColorMatrix type="matrix" result="c" color-interpolation-filters="sRGB"
                       values="0 1 0 0 0  1 0 0 0 0  0 0 1 0 0  0 0 0 1 0"/>
        <feComponentTransfer in="c" result="map" color-interpolation-filters="sRGB">
          <feFuncR type="discrete" tableValues="0 0.25 0.5 0.75 1"/>
          <feFuncG type="discrete" tableValues="1 0.5 0 0.75"/>
          <feFuncA type="discrete" tableValues="1"/>
        </feComponentTransfer>
        <feDisplacementMap in="SourceGraphic" in2="map" scale="0.4375" xChannelSelector="R"
                           yChannelSelector="G" color-interpolation-filters="sRGB"/>)"},
    {"diffuse_distant",
     R"(<feDiffuseLighting surfaceScale="4" diffuseConstant="1" result="l">
          <feDistantLight azimuth="45" elevation="40"/>
        </feDiffuseLighting>
        <feComposite in="SourceGraphic" in2="l" operator="arithmetic" k1="0" k2="1" k3="1"
                     k4="0"/>)"},
    {"specular_spot",
     R"(<feSpecularLighting surfaceScale="3" specularConstant="1.25" specularExponent="8"
                            lighting-color="white" result="s">
          <feSpotLight x="10.5" y="12.25" z="30" pointsAtX="32" pointsAtY="32.5"
                       limitingConeAngle="30"/>
        </feSpecularLighting>
        <feComposite in="SourceGraphic" in2="s" operator="arithmetic" k1="0" k2="1" k3="1"
                     k4="0"/>)"},
    {"diffuse_point",
     R"(<feDiffuseLighting surfaceScale="2.5" diffuseConstant="1.5" lighting-color="#adf"
                           result="l">
          <fePointLight x="50.25" y="13.75" z="18.5"/>
        </feDiffuseLighting>
        <feComposite in="SourceGraphic" in2="l" operator="arithmetic" k1="0" k2="1" k3="1"
                     k4="0"/>)"},
}};

/// \p source, laid out on \p large, rendered on \p device as the four quadrants of its view box,
/// each on its own canvas of half the size, and joined: a reference in which no raster exceeds a
/// quarter of the canvas.
RendererBitmap RenderInQuadrants(const std::shared_ptr<geode::GeodeDevice>& device,
                                 const std::string& source, const LargeCanvas& large) {
  const Vector2i canvas = large.canvas;
  const Vector2i half = canvas / 2;
  RendererBitmap joined;
  joined.dimensions = canvas;
  joined.rowBytes = static_cast<std::size_t>(canvas.x) * 4;
  joined.alphaType = AlphaType::Unpremultiplied;
  joined.pixels.resize(joined.rowBytes * static_cast<std::size_t>(canvas.y));
  const std::string fullViewBox =
      "viewBox=\"" +
      ViewBoxText(large.viewBoxX, large.viewBoxY, large.viewBoxWidth, large.viewBoxHeight) + "\"";
  for (int row = 0; row < 2; ++row) {
    for (int column = 0; column < 2; ++column) {
      std::string quadrant = source;
      const std::size_t at = quadrant.find(fullViewBox);
      EXPECT_THAT(at, testing::Ne(std::string::npos));
      quadrant.replace(at, fullViewBox.size(),
                       "viewBox=\"" +
                           ViewBoxText(large.viewBoxX + column * large.viewBoxWidth / 2.0,
                                       large.viewBoxY + row * large.viewBoxHeight / 2.0,
                                       large.viewBoxWidth / 2.0, large.viewBoxHeight / 2.0) +
                           "\"");
      RendererResourceStats stats;
      const RendererBitmap part = RenderFilterCanvas(device, quadrant, half, std::nullopt, stats);
      EXPECT_THAT(stats, ReducedFilterRasters(0, 1));
      joined.alphaType = part.alphaType;
      for (int y = 0; y < half.y && !part.pixels.empty(); ++y) {
        std::copy_n(part.pixels.begin() + static_cast<std::ptrdiff_t>(y * part.rowBytes),
                    static_cast<std::ptrdiff_t>(half.x) * 4,
                    joined.pixels.begin() +
                        static_cast<std::ptrdiff_t>((row * half.y + y) * joined.rowBytes +
                                                    static_cast<std::size_t>(column * half.x) * 4));
      }
    }
  }
  return joined;
}

class LargeCanvasTilingTest
    : public RendererGeodeTest,
      public testing::WithParamInterface<std::tuple<ReducedFilterCase, LargeCanvas>> {};

/// On 2048 x 2048 and 3840 x 2160 canvases each graph is past what one untiled graph may hold, so
/// it runs in tiles at full resolution, never reduced, and matches the canvas rendered as four
/// quadrants, each a separate raster no larger than a quarter of the canvas.
TEST_P(LargeCanvasTilingTest, TilesMatchTheQuadrantReference) {
  const auto& [filter, large] = GetParam();
  const std::string source = LargeCanvasSource(filter.primitives, large);
  uint64_t tiles = 0;
  RendererResourceStats stats;
  const RendererBitmap tiled =
      RenderOnTiledDevice(source, large.canvas, /*maximumTileExtent=*/0, tiles, stats);
  EXPECT_THAT(tiles, testing::Gt(1u)) << "Premise: the graph does not fit untiled";
  EXPECT_THAT(stats, TiledFilterGraphs(1));
  EXPECT_THAT(stats, ReducedFilterRasters(0, 1));
  ASSERT_THAT(test::CountNonTransparentPixels(tiled), testing::Gt(0u));
  editor::tests::CompareBitmapToBitmap(tiled, RenderInQuadrants(sharedDevice(), source, large),
                                       std::string(filter.name) + "_on_" + std::string(large.name),
                                       editor::tests::PixelmatchIdentityParams());
}

INSTANTIATE_TEST_SUITE_P(
    Filters, LargeCanvasTilingTest,
    testing::Combine(testing::ValuesIn(kExactLargeCanvasFilters),
                     testing::ValuesIn(kLargeCanvases)),
    [](const testing::TestParamInfo<std::tuple<ReducedFilterCase, LargeCanvas>>& info) {
      return std::string(std::get<0>(info.param).name) + "_" +
             std::string(std::get<1>(info.param).name);
    });

/// Noise generated in tiles on a 2048 x 2048 canvas, where it also fits untiled, matches the
/// untiled raster at a tile extent that puts origins at 487, 974 and 1461. Noise is anchored at
/// the filter raster's origin rather than the view box's (#2673), so a quadrant raster would
/// anchor it at its own corner; the untiled raster is the reference instead.
TEST_F(RendererGeodeTest, NoiseOnA2048CanvasMatchesTheUntiledRaster) {
  for (const std::size_t index :
       {std::size_t{7}, std::size_t{8}, std::size_t{9}, std::size_t{10}}) {
    const ReducedFilterCase& filter = kTileableFilters[index];
    SCOPED_TRACE(filter.name);
    const std::string source = WholeCanvasFilterSource(filter.primitives);
    uint64_t untiledTiles = 0;
    uint64_t tiles = 0;
    RendererResourceStats stats;
    const RendererBitmap untiled =
        RenderOnTiledDevice(source, {2048, 2048}, /*maximumTileExtent=*/0, untiledTiles, stats);
    ASSERT_THAT(untiledTiles, testing::Eq(1u)) << "Premise: noise alone fits untiled";
    const RendererBitmap tiled =
        RenderOnTiledDevice(source, {2048, 2048}, /*maximumTileExtent=*/487, tiles, stats);
    ASSERT_THAT(tiles, testing::Gt(1u));
    EXPECT_THAT(stats, TiledFilterGraphs(1));
    editor::tests::CompareBitmapToBitmap(tiled, untiled,
                                         "noise_on_2048_canvas_" + std::string(filter.name),
                                         editor::tests::PixelmatchIdentityParams());
  }
}

/// A turbulence-driven displacement is past the untiled limits on 2048 x 2048 and 3840 x 2160
/// canvases and runs in tiles at full resolution. With no untiled or quadrant reference for noise
/// at that size (see `NoiseOnA2048CanvasMatchesTheUntiledRaster`), a second, finer tiling with
/// different tile origins must agree with it to the pixel.
TEST_F(RendererGeodeTest, TurbulenceDisplacementOnLargeCanvasesRunsInTiles) {
  for (const LargeCanvas& large : kLargeCanvases) {
    SCOPED_TRACE(large.name);
    const std::string source = LargeCanvasSource(kTileableFilters[11].primitives, large);
    uint64_t tiles = 0;
    uint64_t finerTiles = 0;
    RendererResourceStats stats;
    const RendererBitmap tiled =
        RenderOnTiledDevice(source, large.canvas, /*maximumTileExtent=*/0, tiles, stats);
    EXPECT_THAT(tiles, testing::Gt(1u));
    EXPECT_THAT(stats, TiledFilterGraphs(1));
    EXPECT_THAT(stats, ReducedFilterRasters(0, 1));
    ASSERT_THAT(test::CountNonTransparentPixels(tiled), testing::Gt(0u));
    const RendererBitmap retiled =
        RenderOnTiledDevice(source, large.canvas, /*maximumTileExtent=*/383, finerTiles, stats);
    EXPECT_THAT(finerTiles, testing::Gt(tiles));
    EXPECT_THAT(stats, TiledFilterGraphs(1)) << "Premise: the finer tiling runs at full resolution";
    editor::tests::CompareBitmapToBitmap(tiled, retiled,
                                         "turbulence_displacement_on_" + std::string(large.name),
                                         editor::tests::PixelmatchIdentityParams());
  }
}

}  // namespace
}  // namespace donner::svg
