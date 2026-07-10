/// @file
/// Tests for `RendererGeode::setDebugGeometryOverlay` - the Geode band /
/// triangle debug overlay.
///
/// Contract under test:
///  1. Overlay OFF is the default and is byte-identical to a renderer
///     that never touched the flag (zero behavior change when off).
///  2. Overlay ON changes the output and paints the overlay palette
///     (magenta bounding-quad wireframe) over the scene.
///  3. Overlay ON emits additional draw calls; turning it back off
///     restores byte-identical output (no sticky state).

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string_view>

#include "donner/base/ParseWarningSink.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"

namespace donner::svg {
namespace {

/// Fixture with a fill, a stroked path, and a curve so the overlay
/// exercises fill encodes, stroke encodes, and multi-band paths.
constexpr std::string_view kOverlaySvg = R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
  <rect x="20" y="20" width="70" height="70" fill="#336699"/>
  <path d="M 110 30 C 150 10 180 60 160 100 S 120 180 100 150 Z"
        fill="#66aa33" stroke="black" stroke-width="4"/>
</svg>
)SVG";

SVGDocument parseDocument(std::string_view svgSource) {
  ParseWarningSink sink = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(svgSource, sink);
  EXPECT_FALSE(parsed.hasError()) << (parsed.hasError() ? parsed.error().reason : "");
  return std::move(parsed.result());
}

/// Render the fixture and snapshot it. `overlay` selects the debug
/// overlay state; `std::nullopt`-style third state is covered by
/// `renderDefault` which never calls the setter.
RendererBitmap renderWithOverlay(const std::shared_ptr<geode::GeodeDevice>& device, bool overlay) {
  SVGDocument document = parseDocument(kOverlaySvg);
  RendererGeode renderer(device);
  renderer.setDebugGeometryOverlay(overlay);
  renderer.draw(document);
  return renderer.takeSnapshot();
}

/// Render without ever touching the overlay setter.
RendererBitmap renderDefault(const std::shared_ptr<geode::GeodeDevice>& device) {
  SVGDocument document = parseDocument(kOverlaySvg);
  RendererGeode renderer(device);
  renderer.draw(document);
  return renderer.takeSnapshot();
}

bool bitmapsIdentical(const RendererBitmap& a, const RendererBitmap& b) {
  return a.dimensions == b.dimensions && a.rowBytes == b.rowBytes && a.pixels == b.pixels;
}

/// Count pixels in the overlay's magenta family (bounding-quad wireframe:
/// RGBA(255, 0, 255, 200) blended over arbitrary content keeps R and B
/// high and G low).
int countMagentaFamilyPixels(const RendererBitmap& bitmap) {
  int count = 0;
  for (int y = 0; y < bitmap.dimensions.y; ++y) {
    const uint8_t* row = bitmap.pixels.data() + static_cast<size_t>(y) * bitmap.rowBytes;
    for (int x = 0; x < bitmap.dimensions.x; ++x) {
      const uint8_t* px = row + static_cast<size_t>(x) * 4;
      if (px[0] > 150 && px[2] > 150 && px[1] < 80) {
        ++count;
      }
    }
  }
  return count;
}

class GeodeDebugOverlayTest : public ::testing::Test {
protected:
  static std::shared_ptr<geode::GeodeDevice> sharedDevice() {
    static auto device = [] {
      return std::shared_ptr<geode::GeodeDevice>(geode::GeodeDevice::CreateHeadless());
    }();
    return device;
  }
};

TEST_F(GeodeDebugOverlayTest, DefaultsOff) {
  auto device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  RendererGeode renderer(device);
  EXPECT_FALSE(renderer.debugGeometryOverlay());
}

TEST_F(GeodeDebugOverlayTest, OffIsByteIdenticalToDefault) {
  auto device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  const RendererBitmap untouched = renderDefault(device);
  const RendererBitmap explicitlyOff = renderWithOverlay(device, false);

  ASSERT_FALSE(untouched.empty());
  EXPECT_TRUE(bitmapsIdentical(untouched, explicitlyOff))
      << "setDebugGeometryOverlay(false) must not change output vs never calling it.";
}

TEST_F(GeodeDebugOverlayTest, OnDrawsOverlayGeometry) {
  auto device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  const RendererBitmap off = renderWithOverlay(device, false);
  const RendererBitmap on = renderWithOverlay(device, true);

  ASSERT_FALSE(off.empty());
  ASSERT_FALSE(on.empty());
  EXPECT_FALSE(bitmapsIdentical(off, on)) << "Overlay-on output must differ from overlay-off.";

  // The bounding-quad wireframe is magenta; at least a hairline's worth
  // of pixels must land in the magenta family. Overlay-off must have none
  // (the fixture palette has no magenta).
  EXPECT_EQ(countMagentaFamilyPixels(off), 0);
  EXPECT_GT(countMagentaFamilyPixels(on), 50);
}

TEST_F(GeodeDebugOverlayTest, OnEmitsExtraDrawsAndTogglesCleanly) {
  auto device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  SVGDocument document = parseDocument(kOverlaySvg);
  RendererGeode renderer(device);

  renderer.draw(document);
  const RendererBitmap beforeBitmap = renderer.takeSnapshot();
  const uint64_t drawsOff = renderer.lastFrameTimings().counters.drawCalls;

  renderer.setDebugGeometryOverlay(true);
  renderer.draw(document);
  const uint64_t drawsOn = renderer.lastFrameTimings().counters.drawCalls;
  EXPECT_GT(drawsOn, drawsOff) << "Overlay-on frame should emit additional overlay draw calls.";

  // Toggling back off restores byte-identical output on the same renderer.
  renderer.setDebugGeometryOverlay(false);
  renderer.draw(document);
  const RendererBitmap afterBitmap = renderer.takeSnapshot();
  const uint64_t drawsOffAgain = renderer.lastFrameTimings().counters.drawCalls;

  EXPECT_EQ(drawsOffAgain, drawsOff);
  EXPECT_TRUE(bitmapsIdentical(beforeBitmap, afterBitmap))
      << "Disabling the overlay must fully restore non-overlay rendering.";
}

}  // namespace
}  // namespace donner::svg
