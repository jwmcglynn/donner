#include "donner/svg/renderer/RendererGeode.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

#include "donner/base/Box.h"
#include "donner/base/FillRule.h"
#include "donner/base/Path.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/css/Color.h"
#include "donner/svg/components/filter/FilterGraph.h"
#include "donner/svg/properties/PaintServer.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/StrokeParams.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/resources/ImageResource.h"

namespace donner::svg {
namespace {

constexpr double kViewportSize = 64.0;

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

}  // namespace
}  // namespace donner::svg
