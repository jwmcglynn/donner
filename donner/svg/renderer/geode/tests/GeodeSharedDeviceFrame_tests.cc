/// @file
/// Cross-renderer frame-generation claims on one shared `GeodeDevice`.
///
/// Two renderers routinely share a device: an offscreen instance renders an
/// feImage fragment or a layer thumbnail of the SAME document while the outer
/// renderer's frame is still open and has already recorded draws. Frame
/// generations are device-scoped and monotonic, so the inner frame's index
/// never equals the outer's; any "was this touched in the current frame?"
/// equality gate therefore reads the outer frame's resident slots as free.
/// Rewriting a slot's instance record or gradient paint block then corrupts
/// the outer frame retroactively, because every queue write in a frame lands
/// before every draw in that frame's submit.
///
/// Contract under test:
///  1. `GeodeDevice::frameStampClaimed` claims exactly the stamps at or after
///     the oldest still-open generation, and never the never-drawn sentinel.
///  2. End to end: a mid-frame offscreen render of the same document must not
///     repaint pixels an outer frame has already recorded (the outer frame
///     keeps the gradient bytes that were live when its batch was appended).

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>

#include "donner/base/ParseWarningSink.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/shape/ComputedPathComponent.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"

namespace donner::svg {
namespace {

/// A flat "gradient" (both stops the same blue) so a paint-block rewrite is a
/// whole-pixel color change rather than a subtle ramp shift. Gradient fill is
/// required: solid fills read their color from the slot uniform, while a
/// gradient reads the slot's paint block, which is exactly the storage an
/// equality-gated inner pass would rewrite.
constexpr std::string_view kGradientRectSvg = R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100" viewBox="0 0 100 100">
  <defs>
    <linearGradient id="g" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="100" y2="0">
      <stop id="s0" offset="0" stop-color="#0000ff"/>
      <stop id="s1" offset="1" stop-color="#0000ff"/>
    </linearGradient>
  </defs>
  <rect id="target" x="10" y="10" width="60" height="60" fill="url(#g)"/>
</svg>
)SVG";

SVGDocument parseDocument(std::string_view svgSource) {
  ParseWarningSink sink = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(svgSource, sink);
  EXPECT_FALSE(parsed.hasError()) << (parsed.hasError() ? parsed.error().reason : "");
  return std::move(parsed.result());
}

std::array<uint8_t, 4> pixelAt(const RendererBitmap& bitmap, int x, int y) {
  const uint8_t* pixel =
      bitmap.pixels.data() + static_cast<size_t>(y) * bitmap.rowBytes + static_cast<size_t>(x) * 4;
  return {pixel[0], pixel[1], pixel[2], pixel[3]};
}

bool isBlue(const std::array<uint8_t, 4>& px) {
  return px[2] > 200 && px[0] < 60 && px[1] < 60 && px[3] > 200;
}

class GeodeSharedDeviceFrameTest : public ::testing::Test {
protected:
  static std::shared_ptr<geode::GeodeDevice> sharedDevice() {
    static auto device = [] {
      return std::shared_ptr<geode::GeodeDevice>(geode::GeodeDevice::CreateHeadless());
    }();
    return device;
  }
};

// ---------------------------------------------------------------------------
// frameStampClaimed unit contract
// ---------------------------------------------------------------------------

TEST_F(GeodeSharedDeviceFrameTest, FrameStampClaimedTracksOpenGenerations) {
  auto device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  constexpr uint64_t kNeverDrawn = ~uint64_t{0};

  // No open frame: nothing is claimed, including the never-drawn sentinel,
  // which a naive `>=` against `oldestOpenFrameGeneration()`'s `~0` idle
  // value would misread as permanently claimed.
  EXPECT_FALSE(device->frameStampClaimed(kNeverDrawn));
  EXPECT_FALSE(device->frameStampClaimed(0));

  const uint64_t outer = device->beginFrameGeneration();
  // A stamp from the open frame is claimed; earlier (submitted) stamps and
  // the sentinel are not.
  EXPECT_TRUE(device->frameStampClaimed(outer));
  EXPECT_FALSE(device->frameStampClaimed(outer - 1));
  EXPECT_FALSE(device->frameStampClaimed(kNeverDrawn));

  // A nested (offscreen) generation: stamps from EITHER open frame are
  // claimed. This is the cross-renderer case an equality gate gets wrong -
  // `inner != outer` reads "free" while the outer frame's recorded draws
  // still depend on the stamped bytes.
  const uint64_t inner = device->beginFrameGeneration();
  EXPECT_TRUE(device->frameStampClaimed(outer));
  EXPECT_TRUE(device->frameStampClaimed(inner));

  device->endFrameGeneration(inner);
  // The inner frame submitted, the outer is still open: the outer stamp
  // stays claimed, and so does the inner one (it is newer than the oldest
  // open generation, so an open frame could still have recorded against
  // buffers it touched).
  EXPECT_TRUE(device->frameStampClaimed(outer));
  EXPECT_TRUE(device->frameStampClaimed(inner));

  device->endFrameGeneration(outer);
  EXPECT_FALSE(device->frameStampClaimed(outer));
  EXPECT_FALSE(device->frameStampClaimed(inner));
  EXPECT_FALSE(device->frameStampClaimed(kNeverDrawn));
}

// ---------------------------------------------------------------------------
// End-to-end: mid-frame offscreen render of the same document
// ---------------------------------------------------------------------------

/// Drives the outer renderer through the public `RendererInterface` surface
/// exactly as `RendererDriver::traverseRange` does (setPaint from the
/// instance's resolved fill, setTransform from the instance transform,
/// drawPath with the entity-bound `PathShape`), because `draw()` records and
/// submits atomically and the corruption window is BETWEEN those: the outer
/// frame records its gradient batch, an offscreen renderer of the same
/// document then draws mid-frame, and only afterwards does the outer frame
/// submit.
TEST_F(GeodeSharedDeviceFrameTest, MidFrameOffscreenRenderKeepsRecordedGradientPaint) {
  auto device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  SVGDocument document = parseDocument(kGradientRectSvg);
  Registry& registry = document.registry();
  RendererGeode outer(device);

  // Frame 1: a normal draw establishes residence, the entity's record slot,
  // and the gradient paint block, and proves the baseline pixels are blue.
  outer.draw(document);
  {
    const RendererBitmap baseline = outer.takeSnapshot();
    ASSERT_FALSE(baseline.empty());
    const auto px = pixelAt(baseline, 40, 40);
    ASSERT_TRUE(isBlue(px)) << "Baseline gradient rect must render blue, got rgba(" << int(px[0])
                            << "," << int(px[1]) << "," << int(px[2]) << "," << int(px[3]) << ")";
  }

  auto rectElement = document.querySelector("#target");
  ASSERT_TRUE(rectElement.has_value());
  const Entity rectEntity = rectElement->entityHandle().entity();
  const auto& instance = registry.get<components::RenderingInstanceComponent>(rectEntity);
  const auto& computedPath = registry.get<components::ComputedPathComponent>(rectEntity);

  // Frame 2, hand-driven: record the gradient rect the way the driver does.
  RenderViewport viewport;
  viewport.size = Vector2d(100.0, 100.0);
  viewport.devicePixelRatio = 1.0;
  outer.beginFrame(viewport);

  PaintParams gradientPaint;
  gradientPaint.fill = instance.resolvedFill;
  gradientPaint.fillOpacity = 1.0;
  gradientPaint.viewBox = Box2d::FromXYWH(0.0, 0.0, 100.0, 100.0);
  outer.setPaint(gradientPaint);
  outer.setTransform(instance.worldFromEntityTransform);

  PathShape shape;
  shape.path = &computedPath.spline;
  shape.fillRule = FillRule::NonZero;
  shape.sourceEntity = EntityHandle(registry, rectEntity);
  outer.drawPath(shape, StrokeParams{});

  // Record the pending gradient batch into the outer frame's command stream
  // NOW: a solid non-entity draw flushes it (mirroring any later
  // state-changing draw in a real traversal). From here until `endFrame`,
  // the recorded batch draw depends on the entity's record slot and paint
  // block bytes.
  PaintParams cornerPaint;
  cornerPaint.fill = PaintServer::Solid(css::Color(css::RGBA(255, 255, 255, 255)));
  outer.setPaint(cornerPaint);
  outer.setTransform(Transform2d());
  outer.drawRect(Box2d::FromXYWH(90.0, 90.0, 8.0, 8.0), StrokeParams{});

  // Mid-frame: animate the gradient to green and render the same document
  // through an offscreen renderer on the same device (the feImage-fragment /
  // layer-thumbnail shape). Its fresh generation must see the outer frame's
  // claims: if it treats the slots as free it publishes green into the paint
  // block the outer frame's recorded draw reads at submit.
  {
    auto stop0 = document.querySelector("#s0");
    auto stop1 = document.querySelector("#s1");
    ASSERT_TRUE(stop0.has_value());
    ASSERT_TRUE(stop1.has_value());
    stop0->setAttribute("stop-color", "#00ff00");
    stop1->setAttribute("stop-color", "#00ff00");
  }
  std::unique_ptr<RendererInterface> inner = outer.createOffscreenInstance();
  ASSERT_NE(inner, nullptr);
  inner->draw(document);

  outer.endFrame();

  const RendererBitmap result = outer.takeSnapshot();
  ASSERT_FALSE(result.empty());
  const auto px = pixelAt(result, 40, 40);
  EXPECT_TRUE(isBlue(px)) << "Outer frame recorded the gradient as blue before the offscreen "
                             "render; a green pixel means the inner frame rewrote the paint "
                             "block the recorded draw reads at submit. Got rgba("
                          << int(px[0]) << "," << int(px[1]) << "," << int(px[2]) << ","
                          << int(px[3]) << ")";

  // The inner render itself must show the animated green (it drew last with
  // the updated document), proving the mid-frame render really painted G2 and
  // the outer's blue is preservation, not a stale inner pass.
  const RendererBitmap innerResult = static_cast<RendererGeode*>(inner.get())->takeSnapshot();
  ASSERT_FALSE(innerResult.empty());
  const auto innerPx = pixelAt(innerResult, 40, 40);
  EXPECT_TRUE(innerPx[1] > 200 && innerPx[0] < 60 && innerPx[2] < 60)
      << "Offscreen render must show the animated green gradient, got rgba(" << int(innerPx[0])
      << "," << int(innerPx[1]) << "," << int(innerPx[2]) << "," << int(innerPx[3]) << ")";
}

}  // namespace
}  // namespace donner::svg
