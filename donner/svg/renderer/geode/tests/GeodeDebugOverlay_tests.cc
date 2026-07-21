/// @file
/// Tests for `RendererGeode::setDebugGeometryOverlay` - the Slug triangle
/// debug overlay.
///
/// Contract under test:
///  1. Overlay OFF is the default and is byte-identical to a renderer
///     that never touched the flag (zero behavior change when off).
///  2. Overlay ON draws the actual post-vertex Slug triangle edges
///     (`quadVertices` plus dynamic pixel dilation) without tinting normal
///     document pixels between those edges.
///  3. Overlay ON emits one frame-final wireframe draw; turning it back off
///     restores byte-identical output (no sticky state).

#include <gtest/gtest.h>

#include <array>
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

constexpr std::string_view kTextOverlaySvg = R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 100">
  <text x="20" y="76" font-family="sans-serif" font-size="72" fill="#336699">A</text>
</svg>
)SVG";

constexpr std::string_view kLaterPaintOverlaySvg = R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
  <rect x="20" y="20" width="70" height="70" fill="#336699"/>
  <rect width="200" height="200" fill="#f7f8fa"/>
</svg>
)SVG";

constexpr std::string_view kWideStrokeOverlaySvg = R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
  <rect x="40" y="40" width="100" height="100" fill="#336699"
        stroke="#35a16b" stroke-width="24"/>
</svg>
)SVG";

constexpr std::string_view kTransformedOverlaySvg = R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 240 160">
  <rect x="20" y="20" width="60" height="40"
        transform="matrix(2 0 0 1.5 30 25)" fill="#336699"/>
</svg>
)SVG";

constexpr std::string_view kOpacityOverlaySvg = R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
  <g opacity="0.1">
    <rect x="40" y="40" width="100" height="100" fill="#336699"/>
  </g>
</svg>
)SVG";

constexpr std::string_view kUseHeavyOverlaySvg = R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"
     viewBox="0 0 400 100">
  <defs><rect id="r" width="20" height="20" fill="red"/></defs>
  <use xlink:href="#r" x="0"   y="0"/>
  <use xlink:href="#r" x="40"  y="0"/>
  <use xlink:href="#r" x="80"  y="0"/>
  <use xlink:href="#r" x="120" y="0"/>
  <use xlink:href="#r" x="160" y="0"/>
  <use xlink:href="#r" x="200" y="0"/>
  <use xlink:href="#r" x="240" y="0"/>
  <use xlink:href="#r" x="280" y="0"/>
</svg>
)SVG";

constexpr std::string_view kPatternOverlaySvg = R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
  <defs>
    <pattern id="p" patternUnits="userSpaceOnUse" width="40" height="40">
      <rect x="5" y="5" width="10" height="10" fill="#db3a34"/>
    </pattern>
  </defs>
  <rect x="100" y="100" width="60" height="60" fill="url(#p)"/>
</svg>
)SVG";

constexpr std::string_view kGradientClipOverlaySvg = R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 160 140">
  <defs>
    <linearGradient id="g"><stop stop-color="#2f6fed"/><stop offset="1" stop-color="#35a16b"/></linearGradient>
    <clipPath id="c"><rect x="10" y="10" width="70" height="70"/></clipPath>
  </defs>
  <rect x="30" y="30" width="70" height="70" fill="url(#g)" clip-path="url(#c)"/>
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

RendererBitmap renderSvgWithOverlay(const std::shared_ptr<geode::GeodeDevice>& device,
                                    std::string_view svgSource, bool overlay) {
  SVGDocument document = parseDocument(svgSource);
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

std::array<uint8_t, 4> pixelAt(const RendererBitmap& bitmap, int x, int y) {
  const uint8_t* pixel =
      bitmap.pixels.data() + static_cast<size_t>(y) * bitmap.rowBytes + static_cast<size_t>(x) * 4;
  return {pixel[0], pixel[1], pixel[2], pixel[3]};
}

/// Count pixels in the overlay's magenta family. The frame-final wireframe
/// is opaque magenta, while antialiasing over arbitrary content keeps R and
/// B high and G low near its edges.
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

int countNonTransparentPixels(const RendererBitmap& bitmap) {
  int count = 0;
  for (int y = 0; y < bitmap.dimensions.y; ++y) {
    const uint8_t* row = bitmap.pixels.data() + static_cast<size_t>(y) * bitmap.rowBytes;
    for (int x = 0; x < bitmap.dimensions.x; ++x) {
      count += row[static_cast<size_t>(x) * 4 + 3] != 0 ? 1 : 0;
    }
  }
  return count;
}

bool isMagentaFamily(const std::array<uint8_t, 4>& pixel) {
  return pixel[0] > 150 && pixel[2] > 150 && pixel[1] < 80;
}

bool hasMagentaFamilyPixel(const RendererBitmap& bitmap, int x0, int y0, int x1, int y1) {
  for (int y = y0; y <= y1; ++y) {
    for (int x = x0; x <= x1; ++x) {
      if (isMagentaFamily(pixelAt(bitmap, x, y))) {
        return true;
      }
    }
  }
  return false;
}

bool hasPixelDifference(const RendererBitmap& a, const RendererBitmap& b, int x0, int y0, int x1,
                        int y1) {
  for (int y = y0; y <= y1; ++y) {
    for (int x = x0; x <= x1; ++x) {
      if (pixelAt(a, x, y) != pixelAt(b, x, y)) {
        return true;
      }
    }
  }
  return false;
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

TEST_F(GeodeDebugOverlayTest, OffscreenInstanceDoesNotInheritDebugGeometryOverlay) {
  auto device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  RendererGeode renderer(device);
  renderer.setDebugGeometryOverlay(true);
  std::unique_ptr<RendererInterface> offscreen = renderer.createOffscreenInstance();

  ASSERT_NE(offscreen, nullptr);
  EXPECT_FALSE(offscreen->debugGeometryOverlay())
      << "Resource and compositor offscreens must not bake debug geometry into cached pixels";
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

TEST_F(GeodeDebugOverlayTest, OnDrawsActualTriangleEdgesWithoutTintingInterior) {
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

  // The six quadVertices encode two triangles over the source bounds
  // (20,20)-(90,90). The overlay applies the vertex shader's dynamic dilation,
  // so the submitted outer edges move just beyond those bounds while the
  // shared diagonal still crosses (55,55). Point (40,70) remains strictly
  // inside the quad and away from every submitted triangle edge.
  // A real wireframe changes the diagonal and outer edge only. Regressing to
  // a filled bounding quad turns the entire rectangle magenta and fails the
  // interior assertion.
  EXPECT_NE(pixelAt(off, 55, 55), pixelAt(on, 55, 55))
      << "The shared edge between the two emitted Slug triangles must be visible.";
  EXPECT_TRUE(hasPixelDifference(off, on, 90, 52, 92, 58))
      << "The dynamically-dilated Slug triangle's outer edge must be visible.";
  EXPECT_EQ(pixelAt(off, 40, 70), pixelAt(on, 40, 70))
      << "Geometry debug mode must preserve normal pixels away from triangle edges.";
}

TEST_F(GeodeDebugOverlayTest, TextGlyphSlugTrianglesAreIncluded) {
  auto device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  const RendererBitmap off = renderSvgWithOverlay(device, kTextOverlaySvg, false);
  const RendererBitmap on = renderSvgWithOverlay(device, kTextOverlaySvg, true);

  // Prove the fixture produced glyph pixels before using it to test the
  // overlay. With no background, every non-transparent baseline pixel comes
  // from the glyph itself.
  EXPECT_GT(countNonTransparentPixels(off), 200);
  EXPECT_FALSE(bitmapsIdentical(off, on))
      << "Geometry debug mode must capture Slug submissions made by drawText.";
  EXPECT_GT(countMagentaFamilyPixels(on), 20)
      << "The text glyph's emitted Slug triangle edges must be visible.";
}

TEST_F(GeodeDebugOverlayTest, LaterPaintCannotOccludeEarlierTriangleEdges) {
  auto device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  const RendererBitmap off = renderSvgWithOverlay(device, kLaterPaintOverlaySvg, false);
  const RendererBitmap on = renderSvgWithOverlay(device, kLaterPaintOverlaySvg, true);

  // The source bound is x=90 and dynamic dilation moves the submitted edge
  // just outside it. The opaque full-canvas rect painted afterward covers an
  // inline overlay, so only a frame-final pass keeps that edge visible.
  EXPECT_FALSE(hasMagentaFamilyPixel(off, 90, 52, 92, 58));
  EXPECT_TRUE(hasMagentaFamilyPixel(on, 90, 52, 92, 58));
  EXPECT_EQ(pixelAt(off, 40, 70), pixelAt(on, 40, 70))
      << "Final geometry rendering must remain a sparse wireframe.";
}

TEST_F(GeodeDebugOverlayTest, FillTriangleEdgesRenderAboveItsLaterWideStroke) {
  auto device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  const RendererBitmap off = renderSvgWithOverlay(device, kWideStrokeOverlaySvg, false);
  const RendererBitmap on = renderSvgWithOverlay(device, kWideStrokeOverlaySvg, true);

  // The fill bound ends at x=140 and dynamic dilation moves the submitted
  // edge just outside it. The normal stroke covers that edge, while the
  // frame-final overlay restores it above the later stroke.
  EXPECT_FALSE(hasMagentaFamilyPixel(off, 140, 87, 142, 93));
  EXPECT_TRUE(hasMagentaFamilyPixel(on, 140, 87, 142, 93));
  EXPECT_EQ(pixelAt(off, 70, 120), pixelAt(on, 70, 120));
}

TEST_F(GeodeDebugOverlayTest, TriangleEdgesRetainTheSubmittedTransform) {
  auto device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  const RendererBitmap off = renderSvgWithOverlay(device, kTransformedOverlaySvg, false);
  const RendererBitmap on = renderSvgWithOverlay(device, kTransformedOverlaySvg, true);

  // matrix(2,0,0,1.5,30,25) maps the source rect to
  // (70,55)-(190,115). Check both the transformed shared diagonal and outer
  // edge, plus an interior point away from either.
  EXPECT_NE(pixelAt(off, 130, 85), pixelAt(on, 130, 85));
  EXPECT_TRUE(hasPixelDifference(off, on, 190, 82, 192, 88));
  EXPECT_EQ(pixelAt(off, 90, 100), pixelAt(on, 90, 100));
}

TEST_F(GeodeDebugOverlayTest, IsolatedOpacityDoesNotFadeTriangleEdges) {
  auto device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  const RendererBitmap off = renderSvgWithOverlay(device, kOpacityOverlaySvg, false);
  const RendererBitmap on = renderSvgWithOverlay(device, kOpacityOverlaySvg, true);

  EXPECT_FALSE(hasMagentaFamilyPixel(off, 140, 87, 142, 93));
  EXPECT_TRUE(hasMagentaFamilyPixel(on, 140, 87, 142, 93))
      << "Debug geometry must be composited after element opacity.";
  EXPECT_EQ(pixelAt(off, 70, 120), pixelAt(on, 70, 120));
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

TEST_F(GeodeDebugOverlayTest, GeometryOverlayPreservesUseInstancingTopology) {
  auto device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  SVGDocument document = parseDocument(kUseHeavyOverlaySvg);
  RendererGeode renderer(device);
  renderer.setDebugGeometryOverlay(true);
  renderer.draw(document);

  const geode::GeodeCounters counters = renderer.lastFrameTimings().counters;
  EXPECT_EQ(counters.sameSourceDrawPairs, 7u);
  EXPECT_EQ(counters.drawCalls, 2u)
      << "Eight <use> instances stay one real instanced Slug draw plus one final debug draw";
}

TEST_F(GeodeDebugOverlayTest, PatternInternalsStayHiddenWhileConsumerTrianglesAreCaptured) {
  auto device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  const RendererBitmap off = renderSvgWithOverlay(device, kPatternOverlaySvg, false);
  const RendererBitmap on = renderSvgWithOverlay(device, kPatternOverlaySvg, true);

  EXPECT_TRUE(hasPixelDifference(off, on, 126, 126, 134, 134))
      << "The outer pattern-consuming rect's shared Slug edge must be captured";
  EXPECT_FALSE(hasPixelDifference(off, on, 6, 6, 14, 14))
      << "Pattern tile resource geometry must not leak into root debug coordinates";
}

TEST_F(GeodeDebugOverlayTest, GradientAndClipMaskSubmissionPathsAreCaptured) {
  auto device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  const RendererBitmap off = renderSvgWithOverlay(device, kGradientClipOverlaySvg, false);
  const RendererBitmap on = renderSvgWithOverlay(device, kGradientClipOverlaySvg, true);

  EXPECT_TRUE(hasPixelDifference(off, on, 80, 47, 82, 53))
      << "Clip-mask Slug submissions must contribute their post-dilated edge";
  EXPECT_TRUE(hasPixelDifference(off, on, 100, 47, 102, 53))
      << "Gradient Slug submissions must contribute their post-dilated edge even beyond the clip";
  EXPECT_EQ(pixelAt(off, 40, 70), pixelAt(on, 40, 70));
}

}  // namespace
}  // namespace donner::svg
