#include "donner/svg/renderer/RendererGeode.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "donner/base/Box.h"
#include "donner/base/FillRule.h"
#include "donner/base/Path.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/css/Color.h"
#include "donner/svg/components/filter/FilterGraph.h"
#include "donner/svg/properties/PaintServer.h"
#include "donner/svg/renderer/PixelFormatUtils.h"  // IWYU pragma: keep — provides UnpremultiplyRgba
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/StrokeParams.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/resources/ImageResource.h"
#include "tiny_skia/Pixmap.h"
#include "tiny_skia/filter/Lighting.h"

namespace donner::svg {
namespace {

constexpr double kViewportSize = 64.0;

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

/// RGBA pixel at (x, y) in a tightly packed snapshot bitmap.
std::array<uint8_t, 4> pixelAt(const RendererBitmap& bitmap, int x, int y) {
  const size_t off = static_cast<size_t>(y) * bitmap.rowBytes + static_cast<size_t>(x) * 4u;
  return {bitmap.pixels[off], bitmap.pixels[off + 1], bitmap.pixels[off + 2],
          bitmap.pixels[off + 3]};
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

std::vector<uint8_t> CpuSpecularLightingReferenceForFlatSource(
    const components::filter_primitive::SpecularLighting& primitive,
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

  tiny_skia::filter::SpecularLightingParams params;
  params.surfaceScale = primitive.surfaceScale;
  params.specularConstant = primitive.specularConstant;
  params.specularExponent = primitive.specularExponent;
  const css::RGBA rgba = primitive.lightingColor.asRGBA();
  params.lightR = static_cast<double>(rgba.r) / 255.0;
  params.lightG = static_cast<double>(rgba.g) / 255.0;
  params.lightB = static_cast<double>(rgba.b) / 255.0;
  params.pixelToUser = PixelToUserForLighting(deviceFromFilter);
  params.hasShear = HasShearForLighting(deviceFromFilter);
  if (primitive.light.has_value()) {
    params.light = ToTinySkiaLightParams(*primitive.light, deviceFromFilter);
  }

  tiny_skia::filter::specularLighting(*src, *dst, params);
  return UnpremultiplyRgba(dst->data());
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
};

// ----------------------------------------------------------------------------

/// Smoke test: empty frame should snap to a fully transparent bitmap.
TEST_F(RendererGeodeTest, EmptyFrameIsTransparent) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);
  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  ASSERT_FALSE(snap.empty());
  EXPECT_EQ(snap.dimensions.x, static_cast<int>(kViewportSize));
  EXPECT_EQ(snap.dimensions.y, static_cast<int>(kViewportSize));

  auto pixel = pixelAt(snap, 32, 32);
  EXPECT_EQ(pixel[3], 0u) << "Empty frame should be transparent";
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

/// Filling a path with a solid red paint should produce red pixels at the
/// path's interior.
TEST_F(RendererGeodeTest, DrawPathWithSolidFill) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));
  renderer.setTransform(Transform2d());

  PathShape shape;
  shape.path = PathBuilder().addRect(Box2d({16, 16}, {48, 48})).build();
  shape.fillRule = FillRule::NonZero;
  renderer.drawPath(shape, StrokeParams{});

  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  ASSERT_FALSE(snap.empty());

  // Center should be red, transparent at the corner.
  auto center = pixelAt(snap, 32, 32);
  EXPECT_EQ(center[0], 255u) << "Center R";
  EXPECT_EQ(center[1], 0u) << "Center G";
  EXPECT_EQ(center[2], 0u) << "Center B";
  EXPECT_EQ(center[3], 255u) << "Center A";

  auto corner = pixelAt(snap, 4, 4);
  EXPECT_EQ(corner[3], 0u) << "Corner should be transparent";
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
  auto center = pixelAt(snap, 32, 32);
  EXPECT_EQ(center[1], 255u) << "Center G";
  EXPECT_EQ(center[3], 255u) << "Center A";
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
  auto center = pixelAt(snap, 32, 32);
  EXPECT_EQ(center[2], 255u) << "Center B";
  EXPECT_EQ(center[3], 255u) << "Center A";

  // Far corner: outside the inscribed ellipse.
  auto corner = pixelAt(snap, 2, 2);
  EXPECT_EQ(corner[3], 0u) << "Corner should be transparent";
}

/// Transform stack should compose like the other backends. Apply a translate
/// via push, draw, pop, draw — verify both shapes land where expected.
TEST_F(RendererGeodeTest, PushPopTransform) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 255)));

  // Draw a small rect at (8,8)-(16,16) in path space, but with a translate
  // pushed so it lands at (24,24)-(32,32) in pixel space.
  renderer.pushTransform(Transform2d::Translate(Vector2d(16, 16)));
  PathShape shape;
  shape.path = PathBuilder().addRect(Box2d({8, 8}, {16, 16})).build();
  shape.fillRule = FillRule::NonZero;
  renderer.drawPath(shape, StrokeParams{});
  renderer.popTransform();

  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  auto inside = pixelAt(snap, 28, 28);
  EXPECT_EQ(inside[0], 255u) << "Translated rect should cover (28, 28)";

  auto outside = pixelAt(snap, 12, 12);
  EXPECT_EQ(outside[3], 0u) << "Original rect position should be empty";
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

  PathShape shape;
  shape.path = PathBuilder().addRect(Box2d({16, 16}, {48, 48})).build();
  shape.fillRule = FillRule::NonZero;
  renderer.drawPath(shape, stroke);

  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();

  // On the top edge (y=16), well inside the horizontal extent, the stroke
  // should contribute red. The stroke extends outward by width/2 = 2, so
  // any pixel with y in [14, 18) and x in [14, 50) should be touched.
  auto top = pixelAt(snap, 32, 16);
  EXPECT_EQ(top[0], 255u) << "Top edge R";
  EXPECT_EQ(top[1], 0u) << "Top edge G";
  EXPECT_EQ(top[2], 0u) << "Top edge B";

  // The interior of the rect (center) should be transparent — fill=none.
  auto center = pixelAt(snap, 32, 32);
  EXPECT_EQ(center[3], 0u) << "Interior should be transparent (fill=none)";

  // Far corner: outside everything.
  auto corner = pixelAt(snap, 2, 2);
  EXPECT_EQ(corner[3], 0u) << "Corner should be transparent";
}

/// Fill and stroke together: interior should be the fill color, the stroke
/// ring around the edge should be the stroke color.
TEST_F(RendererGeodeTest, FillAndStrokeRect) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  renderer.setPaint(solidFillAndStroke(css::RGBA(0, 255, 0, 255), css::RGBA(0, 0, 255, 255)));
  StrokeParams stroke;
  stroke.strokeWidth = 4.0;

  PathShape shape;
  shape.path = PathBuilder().addRect(Box2d({16, 16}, {48, 48})).build();
  shape.fillRule = FillRule::NonZero;
  renderer.drawPath(shape, stroke);

  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();

  // Deep inside: fill (green) only.
  auto inside = pixelAt(snap, 32, 32);
  EXPECT_EQ(inside[0], 0u) << "Interior R";
  EXPECT_EQ(inside[1], 255u) << "Interior G";
  EXPECT_EQ(inside[2], 0u) << "Interior B";

  // Exactly on the top edge of the rect (y=16), the stroke straddles both
  // sides by width/2 = 2, so this pixel is inside the stroke ring → blue.
  auto topEdge = pixelAt(snap, 32, 16);
  EXPECT_EQ(topEdge[2], 255u) << "Top edge should be in stroke (B)";

  // Far outside the rect is still transparent.
  auto corner = pixelAt(snap, 2, 2);
  EXPECT_EQ(corner[3], 0u) << "Corner should be transparent";
}

/// A semi-transparent fill must round-trip through `takeSnapshot` in
/// straight alpha, not premultiplied. Regression guard for #492 review
/// comment P2: `GeoEncoder::fillPath` premultiplies the paint color by
/// alpha before upload (because the pipeline blend state is
/// premultiplied-source-over), so `takeSnapshot` must unpremultiply when
/// building the straight-alpha `RendererBitmap` — otherwise semi-transparent
/// content comes out darkened and cross-backend parity breaks.
TEST_F(RendererGeodeTest, SnapshotReturnsStraightAlpha) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  // 50% red: R=255, A=128. A straight-alpha round-trip should preserve
  // R~=255 and A~=128. A broken read-back would return the premultiplied
  // RGB (~128,0,0,128) instead — exactly the regression we're guarding.
  renderer.setPaint(solidFill(css::RGBA(255, 0, 0, 128)));
  renderer.drawRect(Box2d({16, 16}, {48, 48}), StrokeParams{});
  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  auto center = pixelAt(snap, 32, 32);
  EXPECT_NEAR(center[0], 255u, 2u) << "Straight-alpha R should be ~255";
  EXPECT_EQ(center[1], 0u) << "G";
  EXPECT_EQ(center[2], 0u) << "B";
  EXPECT_NEAR(center[3], 128u, 2u) << "Alpha preserved";
}

/// Drawing a stroked path when `impl_->encoder` is null (e.g.,
/// draw-before-beginFrame) must be a safe no-op. Regression guard for
/// #492 review comment P1: the stroke branch of `drawPath` previously
/// dereferenced `impl_->encoder` unconditionally, which crashed when the
/// encoder hadn't been created yet. Before the fix, this test would
/// segfault; after the fix, it returns cleanly.
TEST_F(RendererGeodeTest, StrokeBeforeBeginFrameIsNoOp) {
  RendererGeode renderer = createRenderer();
  // Intentionally skip beginFrame — encoder remains null.
  renderer.setPaint(solidStroke(css::RGBA(255, 0, 0, 255)));
  StrokeParams stroke;
  stroke.strokeWidth = 4.0;

  PathShape shape;
  shape.path = PathBuilder().addRect(Box2d({16, 16}, {48, 48})).build();
  shape.fillRule = FillRule::NonZero;
  // Before the fix, this call crashes with a null pointer dereference.
  renderer.drawPath(shape, stroke);
  // No explicit assertion — reaching this line means we didn't crash.
}

/// Stroke with stroke-width 0 should no-op (neither stroke nor warning).
TEST_F(RendererGeodeTest, ZeroWidthStrokeIsNoOp) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  renderer.setPaint(solidStroke(css::RGBA(255, 0, 0, 255)));
  StrokeParams stroke;
  stroke.strokeWidth = 0.0;  // Must skip stroke path entirely.

  PathShape shape;
  shape.path = PathBuilder().addRect(Box2d({16, 16}, {48, 48})).build();
  shape.fillRule = FillRule::NonZero;
  renderer.drawPath(shape, stroke);

  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  auto center = pixelAt(snap, 32, 32);
  EXPECT_EQ(center[3], 0u) << "Zero-width stroke should draw nothing";
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
  params.opacity = 1.0;
  params.imageRenderingPixelated = true;

  renderer.drawImage(image, params);
  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  ASSERT_FALSE(snap.empty());

  // Top-left quadrant (pixel at ~24, 24) — red.
  auto tl = pixelAt(snap, 24, 24);
  EXPECT_EQ(tl[0], 255u) << "TL R";
  EXPECT_EQ(tl[1], 0u) << "TL G";
  EXPECT_EQ(tl[3], 255u) << "TL A";

  // Top-right quadrant (~40, 24) — green.
  auto tr = pixelAt(snap, 40, 24);
  EXPECT_EQ(tr[0], 0u) << "TR R";
  EXPECT_EQ(tr[1], 255u) << "TR G";

  // Bottom-left (~24, 40) — blue.
  auto bl = pixelAt(snap, 24, 40);
  EXPECT_EQ(bl[2], 255u) << "BL B";

  // Bottom-right (~40, 40) — yellow.
  auto br = pixelAt(snap, 40, 40);
  EXPECT_EQ(br[0], 255u) << "BR R";
  EXPECT_EQ(br[1], 255u) << "BR G";

  // Outside the target rect: transparent.
  auto outside = pixelAt(snap, 4, 4);
  EXPECT_EQ(outside[3], 0u) << "Outside alpha";
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
  params.opacity = 1.0;

  renderer.pushTransform(Transform2d::Translate(Vector2d(16, 16)));
  renderer.drawImage(image, params);
  renderer.popTransform();

  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  // Translated pixel should be magenta.
  auto inside = pixelAt(snap, 20, 20);
  EXPECT_EQ(inside[0], 255u) << "Translated R";
  EXPECT_EQ(inside[2], 255u) << "Translated B";
  EXPECT_EQ(inside[3], 255u) << "Translated A";

  // Original (unshifted) position should be empty.
  auto unshifted = pixelAt(snap, 4, 4);
  EXPECT_EQ(unshifted[3], 0u) << "Unshifted origin should be transparent";
}

/// `ImageParams::opacity` controls fade at the draw call; ancestor
/// `opacity` attributes are applied by the driver via
/// `pushIsolatedLayer`, *not* by multiplying `PaintParams::opacity`
/// into the draw itself. This test verifies both channels:
///  * `paint.opacity` alone does NOT attenuate a direct `drawImage`
///    (because it only takes effect at layer composite time)
///  * `params.opacity = 0.5` on its own halves the output alpha
TEST_F(RendererGeodeTest, DrawImageCombinedOpacity) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  // paint.opacity is intentionally set but should NOT affect the raster:
  // it is only honored by the driver's group-opacity path
  // (pushIsolatedLayer/popIsolatedLayer). params.opacity = 0.5 is the
  // only channel that fades the draw here.
  PaintParams paint;
  paint.opacity = 0.5;
  renderer.setPaint(paint);

  ImageResource image;
  image.width = 1;
  image.height = 1;
  image.data = {255, 0, 0, 255};

  ImageParams params;
  params.targetRect = Box2d({16.0, 16.0}, {48.0, 48.0});
  params.opacity = 0.5;

  renderer.drawImage(image, params);
  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  auto center = pixelAt(snap, 32, 32);
  // Straight-alpha: R=255, A≈128 (255 * 0.5 from params.opacity only).
  EXPECT_NEAR(center[0], 255u, 2u) << "Straight-alpha R preserved";
  EXPECT_NEAR(center[3], 128u, 2u) << "params.opacity applied once";
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
  auto center = pixelAt(snap, 32, 32);
  EXPECT_EQ(center[3], 0u) << "Empty image should draw nothing";
}

/// Popping an isolated layer with a non-Normal blend mode while an outer
/// clip is active must NOT clobber backdrop pixels outside the clip rect.
/// This is the Phase 3d regression guarded by loading (not clearing) the
/// parent attachment on the blend-pop pass — a clear would wipe
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
  auto outside = pixelAt(snap, 16, 48);
  EXPECT_EQ(outside[0], 255u) << "Bottom-left R (outside clip)";
  EXPECT_EQ(outside[1], 0u) << "Bottom-left G (outside clip)";
  EXPECT_EQ(outside[2], 0u) << "Bottom-left B (outside clip)";
  EXPECT_EQ(outside[3], 255u) << "Bottom-left A (outside clip)";

  // Inside the clip: Multiply(red=255,0,0 ; blue=0,0,255) = (0, 0, 0).
  auto inside = pixelAt(snap, 48, 16);
  EXPECT_EQ(inside[0], 0u) << "Multiply result R";
  EXPECT_EQ(inside[1], 0u) << "Multiply result G";
  EXPECT_EQ(inside[2], 0u) << "Multiply result B";
  EXPECT_EQ(inside[3], 255u) << "Multiply result A";
}

/// Phase 3b repro: an arbitrary path clip should gate color draws to the mask.
/// A full-viewport blue rect clipped by a left-half rect encoded as a PATH
/// must render blue only on the left half.
TEST_F(RendererGeodeTest, PathClipMaskClipsSolidFillToLeftHalf) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  ResolvedClip clip;
  PathShape clipShape;
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

  auto inside = pixelAt(snap, 16, 32);
  EXPECT_EQ(inside[0], 0u) << "Inside clip R";
  EXPECT_EQ(inside[1], 0u) << "Inside clip G";
  EXPECT_EQ(inside[2], 255u) << "Inside clip B";
  EXPECT_EQ(inside[3], 255u) << "Inside clip A";

  auto outside = pixelAt(snap, 48, 32);
  EXPECT_EQ(outside[0], 0u) << "Outside clip R";
  EXPECT_EQ(outside[1], 0u) << "Outside clip G";
  EXPECT_EQ(outside[2], 0u) << "Outside clip B";
  EXPECT_EQ(outside[3], 0u) << "Outside clip A";
}

TEST_F(RendererGeodeTest, PathClipMaskClipsIsolatedLayerComposite) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  ResolvedClip clip;
  PathShape clipShape;
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

  auto inside = pixelAt(snap, 16, 32);
  EXPECT_EQ(inside[0], 0u) << "Inside clip R";
  EXPECT_EQ(inside[1], 0u) << "Inside clip G";
  EXPECT_EQ(inside[2], 255u) << "Inside clip B";
  EXPECT_EQ(inside[3], 255u) << "Inside clip A";

  auto outside = pixelAt(snap, 48, 32);
  EXPECT_EQ(outside[0], 0u) << "Outside clip R";
  EXPECT_EQ(outside[1], 0u) << "Outside clip G";
  EXPECT_EQ(outside[2], 0u) << "Outside clip B";
  EXPECT_EQ(outside[3], 0u) << "Outside clip A";
}

TEST_F(RendererGeodeTest, PathClipMaskClipsIsolatedLayerCompositeForTriangle) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  ResolvedClip clip;
  PathShape clipShape;
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

  auto inside = pixelAt(snap, 32, 40);
  EXPECT_EQ(inside[0], 0u) << "Inside clip R";
  EXPECT_EQ(inside[1], 0u) << "Inside clip G";
  EXPECT_EQ(inside[2], 255u) << "Inside clip B";
  EXPECT_EQ(inside[3], 255u) << "Inside clip A";

  auto outside = pixelAt(snap, 8, 8);
  EXPECT_EQ(outside[0], 0u) << "Outside clip R";
  EXPECT_EQ(outside[1], 0u) << "Outside clip G";
  EXPECT_EQ(outside[2], 0u) << "Outside clip B";
  EXPECT_EQ(outside[3], 0u) << "Outside clip A";
}

TEST_F(RendererGeodeTest, PathClipMaskClipsFilterLayerCompositeForTriangle) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  ResolvedClip clip;
  PathShape clipShape;
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

  auto inside = pixelAt(snap, 32, 40);
  EXPECT_EQ(inside[0], 0u) << "Inside clip R";
  EXPECT_EQ(inside[1], 0u) << "Inside clip G";
  EXPECT_EQ(inside[2], 255u) << "Inside clip B";
  EXPECT_EQ(inside[3], 255u) << "Inside clip A";

  auto outside = pixelAt(snap, 8, 8);
  EXPECT_EQ(outside[0], 0u) << "Outside clip R";
  EXPECT_EQ(outside[1], 0u) << "Outside clip G";
  EXPECT_EQ(outside[2], 0u) << "Outside clip B";
  EXPECT_EQ(outside[3], 0u) << "Outside clip A";
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
  renderer.pushMask(std::nullopt);
  renderer.transitionMaskToContent();
  renderer.popMask();

  // After all that, a normal draw should still work.
  renderer.setPaint(solidFill(css::RGBA(255, 255, 0, 255)));
  renderer.drawRect(Box2d({16, 16}, {48, 48}), StrokeParams{});

  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  auto center = pixelAt(snap, 32, 32);
  EXPECT_EQ(center[0], 255u);
  EXPECT_EQ(center[1], 255u);
  EXPECT_EQ(center[3], 255u);
}

/// Smoke test for Phase 7 feGaussianBlur filter layer. Draws a crisp red
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
  auto center = pixelAt(snap, 32, 32);
  EXPECT_EQ(center[0], 255u) << "Center red channel should be fully opaque";
  EXPECT_GT(center[3], 200u) << "Center alpha should still be high";

  // An edge pixel (just inside the rect boundary) should have reduced
  // alpha compared to the center, proving the blur was applied.
  auto edge = pixelAt(snap, 16, 32);
  EXPECT_LT(edge[3], center[3]) << "Edge pixel alpha should be less than center (blur applied)";
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
  auto center = pixelAt(snap, 32, 32);
  EXPECT_EQ(center[1], 255u) << "Green channel at center";
  EXPECT_EQ(center[3], 255u) << "Alpha at center should be fully opaque";

  // Just outside the rect should be transparent (no blur spread).
  auto outside = pixelAt(snap, 14, 32);
  EXPECT_EQ(outside[3], 0u) << "Outside the rect should be transparent with zero blur";
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
  auto shifted = pixelAt(snap, 36, 36);
  EXPECT_EQ(shifted[0], 255u) << "Shifted center should be red";
  EXPECT_EQ(shifted[3], 255u) << "Shifted center alpha";

  // Original corner of the rect at (17,17) should now be transparent because
  // the offset shifted content down-right.
  auto original = pixelAt(snap, 17, 17);
  EXPECT_EQ(original[3], 0u) << "Original top-left should be transparent after offset";
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
  auto center = pixelAt(snap, 32, 32);
  EXPECT_EQ(center[0], 0u) << "R channel should be 0 (luminanceToAlpha)";
  EXPECT_EQ(center[1], 0u) << "G channel should be 0 (luminanceToAlpha)";
  EXPECT_EQ(center[2], 0u) << "B channel should be 0 (luminanceToAlpha)";
  EXPECT_NEAR(center[3], 54u, 3u) << "Alpha should match Y-channel luminance of red";

  // Outside the rect: transparent.
  auto outside = pixelAt(snap, 4, 4);
  EXPECT_EQ(outside[3], 0u) << "Outside should be transparent";
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

  const auto center = pixelAt(snap, 32, 32);
  EXPECT_EQ(center[0], 0u);
  EXPECT_EQ(center[1], 0u);
  EXPECT_EQ(center[2], 0u);
  EXPECT_EQ(center[3], 255u);
}

TEST_F(RendererGeodeTest, FilterSpecularLightingPointLightExponentZeroMatchesCpuReference) {
  components::FilterGraph graph;
  components::FilterNode specularNode;
  components::filter_primitive::SpecularLighting specular;
  specular.surfaceScale = 3.0;
  specular.specularConstant = 1.0;
  specular.specularExponent = 0.0;
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
  const std::vector<uint8_t> expected =
      CpuSpecularLightingReferenceForFlatSource(specular, deviceFromFilter);
  ASSERT_FALSE(expected.empty());

  const BitmapDiffStats diff = DiffBitmapAgainstStraightRgba(actual, expected);
  EXPECT_EQ(diff.differingChannels, 0)
      << "maxDiff=" << diff.maxChannelDiff << " first=(" << diff.firstDiffX << ", "
      << diff.firstDiffY << ") channel=" << diff.firstDiffChannel
      << " actual=" << static_cast<int>(diff.actualValue)
      << " expected=" << static_cast<int>(diff.expectedValue);
}

TEST_F(RendererGeodeTest, FilterDiffuseLightingSpotLightConeMatchesCpuReference) {
  components::FilterGraph graph;
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

  const auto center = pixelAt(snap, 32, 32);
  EXPECT_EQ(center[0], 0u);
  EXPECT_EQ(center[1], 0u);
  EXPECT_EQ(center[2], 0u);
  EXPECT_EQ(center[3], 0u);
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
  auto center = pixelAt(snap, 32, 32);
  EXPECT_NEAR(center[0], 255u, 2u) << "Flood R (straight alpha)";
  EXPECT_EQ(center[1], 0u) << "Flood G";
  EXPECT_EQ(center[2], 0u) << "Flood B";
  EXPECT_NEAR(center[3], 128u, 2u) << "Flood A (50% opacity)";
}

/// feMerge: composite two feFlood layers via alpha-over.
TEST_F(RendererGeodeTest, FilterMergeCompositesInputs) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  // Build a filter graph:
  //   Node 0: feFlood red 50% → result="red"
  //   Node 1: feFlood blue 50% → result="blue"
  //   Node 2: feMerge(in="red", in="blue") → alpha-over composite
  components::FilterGraph graph;

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
  auto center = pixelAt(snap, 32, 32);
  EXPECT_NEAR(center[0], 85u, 6u) << "Merge R (straight, red behind blue)";
  EXPECT_EQ(center[1], 0u) << "Merge G";
  EXPECT_NEAR(center[2], 170u, 6u) << "Merge B (straight, blue on top)";
  EXPECT_GT(center[3], 170u) << "Merge A should be > 170 (composited alpha)";
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
  auto center = pixelAt(snap, 32, 32);
  EXPECT_NEAR(center[0], 255u, 3u) << "Composite-in R (straight)";
  EXPECT_EQ(center[1], 0u) << "Composite-in G";
  EXPECT_EQ(center[2], 0u) << "Composite-in B";
  EXPECT_NEAR(center[3], 128u, 3u) << "Composite-in A";
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

  // in1 = blue 50% premul (0,0,128,128), in2 = red opaque (255,0,0,255).
  // over: result = in1 + in2 * (1 - in1.a) = (0,0,128,128) + (255,0,0,255)*(1 - 0.502)
  //     ≈ (0,0,128,128) + (127,0,0,127) = (127,0,128,255).
  // Straight: R≈127, G=0, B≈128, A=255.
  auto center = pixelAt(snap, 32, 32);
  EXPECT_NEAR(center[0], 127u, 4u) << "Composite-over R";
  EXPECT_EQ(center[1], 0u) << "Composite-over G";
  EXPECT_NEAR(center[2], 128u, 4u) << "Composite-over B";
  EXPECT_NEAR(center[3], 255u, 1u) << "Composite-over A";
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
  auto center = pixelAt(snap, 32, 32);
  EXPECT_NEAR(center[0], 255u, 2u) << "Arithmetic R (red*white=red)";
  EXPECT_EQ(center[1], 0u) << "Arithmetic G";
  EXPECT_EQ(center[2], 0u) << "Arithmetic B";
  EXPECT_NEAR(center[3], 255u, 2u) << "Arithmetic A";
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
  auto center = pixelAt(snap, 32, 32);
  EXPECT_EQ(center[0], 0u) << "Blend-multiply R";
  EXPECT_EQ(center[1], 0u) << "Blend-multiply G";
  EXPECT_EQ(center[2], 0u) << "Blend-multiply B";
  EXPECT_NEAR(center[3], 255u, 1u) << "Blend-multiply A";
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
  auto center = pixelAt(snap, 32, 32);
  EXPECT_NEAR(center[0], 255u, 1u) << "Blend-screen R";
  EXPECT_EQ(center[1], 0u) << "Blend-screen G";
  EXPECT_NEAR(center[2], 255u, 1u) << "Blend-screen B";
  EXPECT_NEAR(center[3], 255u, 1u) << "Blend-screen A";
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
  auto expanded = pixelAt(snap, 19, 32);
  EXPECT_GT(expanded[3], 200u) << "Dilate should expand the shape outward";
  EXPECT_GT(expanded[0], 200u) << "Dilated pixel should be white (R)";

  // Center should still be white.
  auto center = pixelAt(snap, 32, 32);
  EXPECT_EQ(center[0], 255u) << "Center should remain white";
  EXPECT_EQ(center[3], 255u) << "Center alpha should be fully opaque";
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
  auto corner = pixelAt(snap, 17, 17);
  EXPECT_EQ(corner[3], 0u) << "Corner should be transparent after erode";

  // Center (32, 32) should still be white (far from edges).
  auto center = pixelAt(snap, 32, 32);
  EXPECT_EQ(center[0], 255u) << "Center should remain white";
  EXPECT_EQ(center[3], 255u) << "Center alpha";
}

/// feComponentTransfer identity: all channels pass through unchanged.
TEST_F(RendererGeodeTest, FilterComponentTransferIdentityPasses) {
  RendererGeode renderer = createRenderer();
  beginFrame(renderer);

  components::FilterGraph graph;
  components::FilterNode ctNode;
  components::filter_primitive::ComponentTransfer ct;
  // All 4 channels default to Identity — no explicit setup needed.
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

  auto center = pixelAt(snap, 32, 32);
  EXPECT_NEAR(center[0], 128u, 2u) << "R should pass through";
  EXPECT_NEAR(center[1], 64u, 2u) << "G should pass through";
  EXPECT_NEAR(center[2], 200u, 2u) << "B should pass through";
  EXPECT_NEAR(center[3], 255u, 1u) << "A should pass through";
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

  auto center = pixelAt(snap, 32, 32);
  // R=128/255≈0.502, R²≈0.252, premultiplied: R*A=0.252*1.0=0.252 → ~64.
  EXPECT_NEAR(center[0], 64u, 4u) << "R should be squared (~64)";
  EXPECT_EQ(center[1], 0u) << "G should be zero";
  EXPECT_EQ(center[2], 0u) << "B should be zero";
  EXPECT_NEAR(center[3], 255u, 1u) << "A unchanged";
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
  auto center = pixelAt(snap, 32, 32);
  EXPECT_GT(center[0], 240u) << "Center R should be near-white after box blur";

  // Edge pixel (16, 32): 3×3 kernel straddles the edge, so the value
  // should be intermediate (not fully white, not fully transparent).
  auto edge = pixelAt(snap, 16, 32);
  EXPECT_GT(edge[3], 50u) << "Edge pixel should have some alpha (box blur averages)";
  EXPECT_LT(edge[3], 250u) << "Edge pixel should be less than center";
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
  auto interior = pixelAt(snap, 32, 32);
  EXPECT_LT(interior[0], 20u) << "Interior should be near-black (Laplacian=0)";
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
  PathShape clipShape;
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
  // pixel — being inside the clip — must remain near-opaque blue. The buggy
  // path captures only the left-half blue and the blur averages with
  // transparent-black, so the alpha drops to ~50% (~177) here.
  auto nearEdgeInside = pixelAt(snap, 30, 32);
  EXPECT_GT(nearEdgeInside[3], 240u)
      << "Near-edge inside-clip alpha should remain near-opaque (≥240). The "
         "buggy path blurs the already-clipped half-rect and pulls alpha "
         "toward ~50% here. Got "
      << static_cast<int>(nearEdgeInside[3]);

  // (34, 32): just OUTSIDE the clip rect. Per §15.5 the clip is applied to
  // the FILTERED result, so this pixel must be fully transparent regardless
  // of the blur radius. The buggy path leaks blur energy outside the clip
  // boundary because the composite isn't gated by the clip mask, leaving
  // alpha around 50.
  auto justOutside = pixelAt(snap, 34, 32);
  EXPECT_EQ(justOutside[3], 0u)
      << "Just-outside-clip alpha must be 0 — clip-path is applied to the "
         "filtered result. Got "
      << static_cast<int>(justOutside[3]);

  // (8, 32): well inside the clipped region, far from the boundary. Both
  // correct and buggy paths produce near-opaque blue here, but check anyway
  // as a sanity gate.
  auto deepInside = pixelAt(snap, 8, 32);
  EXPECT_GT(deepInside[2], 230u) << "Deep-inside B should be ~255 (blue)";
  EXPECT_GT(deepInside[3], 230u) << "Deep-inside A should be near-opaque";

  // (48, 32): well outside the clip rect, far from the boundary. The clip is
  // applied AFTER the filter, so this pixel must be fully transparent.
  auto outside = pixelAt(snap, 48, 32);
  EXPECT_EQ(outside[3], 0u) << "Outside the clip must be transparent — the "
                               "filter result is clipped on composite";
}

}  // namespace
}  // namespace donner::svg
