#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/Transform.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/compositor/CompositorController.h"
#include "donner/svg/compositor/DualPathVerifier.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/Renderer.h"

namespace donner::svg::compositor {

namespace {

struct Pixel {
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
  uint8_t a = 0;

  friend std::ostream& operator<<(std::ostream& os, const Pixel& p) {
    return os << "Pixel(" << int(p.r) << "," << int(p.g) << "," << int(p.b) << "," << int(p.a)
              << ")";
  }
};

Pixel getPixel(const RendererBitmap& bitmap, int x, int y) {
  if (x < 0 || x >= bitmap.dimensions.x || y < 0 || y >= bitmap.dimensions.y) {
    return {};
  }
  const size_t offset = static_cast<size_t>(y) * bitmap.rowBytes + static_cast<size_t>(x) * 4;
  return {bitmap.pixels[offset], bitmap.pixels[offset + 1], bitmap.pixels[offset + 2],
          bitmap.pixels[offset + 3]};
}

MATCHER(IsRed, "") {
  *result_listener << "pixel is " << arg;
  return arg.r > 200 && arg.g < 50 && arg.b < 50 && arg.a > 200;
}

MATCHER(IsWhite, "") {
  *result_listener << "pixel is " << arg;
  return arg.r > 200 && arg.g > 200 && arg.b > 200 && arg.a > 200;
}

SVGDocument parseDocument(std::string_view svgSource) {
  ParseWarningSink sink;
  auto result = parser::SVGParser::ParseSVG(svgSource, sink);
  EXPECT_FALSE(result.hasError()) << result.error().reason;
  return std::move(result).result();
}

class CompositorGoldenTest : public ::testing::Test {
protected:
  svg::Renderer renderer_;
  RenderViewport viewport_;

  CompositorGoldenTest() {
    viewport_.size = Vector2d(200, 100);
    viewport_.devicePixelRatio = 1.0;
  }
};

}  // namespace

// Composited output of a simple document matches the full-render output at
// every pixel. Baseline correctness: no compositor optimization preserves
// correctness if this fails.
TEST_F(CompositorGoldenTest, PixelIdentityOnSimpleDocument) {
  SVGDocument document = parseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
      <rect width="200" height="100" fill="white"/>
      <rect x="10" y="10" width="50" height="50" fill="red"/>
    </svg>
  )svg");

  CompositorController compositor(document, renderer_);
  DualPathVerifier verifier(compositor, renderer_);
  const auto result = verifier.renderAndVerify(viewport_);

  EXPECT_TRUE(result.isExact()) << result;
  EXPECT_EQ(result.mismatchCount, 0u);
  EXPECT_EQ(result.maxChannelDiff, 0);
}

// With an entity explicitly promoted to its own layer, compositor output is
// pixel-identical to the full render. Covers the layer-rasterize + compose
// round-trip.
TEST_F(CompositorGoldenTest, PromotedEntityMatchesFullRender) {
  SVGDocument document = parseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
      <rect width="200" height="100" fill="white"/>
      <rect id="target" x="10" y="10" width="50" height="50" fill="red"/>
    </svg>
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  CompositorController compositor(document, renderer_);
  ASSERT_TRUE(compositor.promoteEntity(target->entityHandle().entity()));

  DualPathVerifier verifier(compositor, renderer_);
  const auto result = verifier.renderAndVerify(viewport_);

  EXPECT_TRUE(result.isExact()) << result;
}

// Translation-only drag: composited fast path produces the same pixels as a
// full re-render with the `transform` attribute updated. This is the core
// fluid-drag invariant.
TEST_F(CompositorGoldenTest, TranslationOnlyDragProducesCorrectPixels) {
  SVGDocument document = parseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
      <rect width="200" height="100" fill="white"/>
      <rect id="target" x="10" y="10" width="50" height="50" fill="red"/>
    </svg>
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->entityHandle().entity();

  CompositorController compositor(document, renderer_);
  ASSERT_TRUE(compositor.promoteEntity(entity));
  compositor.setLayerCompositionTransform(entity, Transform2d::Translate(Vector2d(100.0, 0.0)));
  compositor.renderFrame(viewport_);

  const RendererBitmap flat = renderer_.takeSnapshot();
  EXPECT_THAT(getPixel(flat, 135, 35), IsRed()) << "Red rect at translated position";
  EXPECT_THAT(getPixel(flat, 35, 35), IsWhite()) << "Original position now vacated";
}

// The ancestor-clip safety check (committed earlier) should prevent
// auto-promotion here, so the output exactly matches the full render — no
// layer extraction, no lost clip context.
TEST_F(CompositorGoldenTest, ClippedGroupWithOpacityChildRendersCorrectly) {
  SVGDocument document = parseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
      <defs>
        <clipPath id="half">
          <rect x="0" y="0" width="100" height="100"/>
        </clipPath>
      </defs>
      <rect width="200" height="100" fill="white"/>
      <g clip-path="url(#half)">
        <rect x="50" y="10" width="100" height="80" fill="red" opacity="0.5"/>
      </g>
    </svg>
  )svg");

  CompositorController compositor(document, renderer_);
  DualPathVerifier verifier(compositor, renderer_);
  const auto result = verifier.renderAndVerify(viewport_);

  // Pixel identity with the full-render path. If the child were auto-promoted
  // despite the ancestor clip, the composited path would leak rect content
  // past x=100 and this assertion would catch it.
  EXPECT_TRUE(result.isExact()) << result;
  EXPECT_EQ(result.mismatchCount, 0u);
}

}  // namespace donner::svg::compositor
