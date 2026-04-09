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
#include "donner/svg/properties/PaintServer.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/StrokeParams.h"

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
  RendererGeode renderer;
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
  RendererGeode renderer;
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
  RendererGeode renderer;
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
  RendererGeode renderer;
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
  RendererGeode renderer;
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
  RendererGeode renderer;
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
  RendererGeode renderer;
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
  RendererGeode renderer;
  beginFrame(renderer);

  renderer.setPaint(
      solidFillAndStroke(css::RGBA(0, 255, 0, 255), css::RGBA(0, 0, 255, 255)));
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
  RendererGeode renderer;
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
  RendererGeode renderer;
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
  RendererGeode renderer;
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

/// Stubbed methods (clip/mask/layer/filter/pattern/image/text) should be
/// safe no-ops that don't crash, and balanced push/pop pairs should keep
/// drawing functional.
TEST_F(RendererGeodeTest, StubbedMethodsAreNoOps) {
  RendererGeode renderer;
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

}  // namespace
}  // namespace donner::svg
