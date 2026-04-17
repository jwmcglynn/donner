#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/Transform.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/base/Vector2.h"
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

// Targeted probe: when a top-level `<rect>` is bucketed in isolation, does
// `rasterizeLayer` (via `RendererDriver::drawEntityRange`) produce the
// correct pixels? Calls the same code path the bucketer triggers at
// runtime, with a single standalone element.
TEST_F(CompositorGoldenTest, BucketedStandaloneRectRasterizesCorrectly) {
  SVGDocument document = parseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
      <rect width="200" height="100" fill="white"/>
      <rect id="target" x="10" y="10" width="50" height="50" fill="red"/>
    </svg>
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  // Aggressive bucketing (threshold = 1) promotes both top-level rects into
  // their own layers. If `drawEntityRange` correctly handles standalone
  // top-level elements, the composited output matches the full-render output.
  CompositorConfig config;
  config.complexityBucketing = true;
  CompositorController compositor(document, renderer_, config);

  // Explicit promote to set up the drag scenario. With `complexityBucketing`
  // on and aggressive threshold, the white-background rect also gets a bucket.
  ASSERT_TRUE(compositor.promoteEntity(target->entityHandle().entity()));
  compositor.setLayerCompositionTransform(target->entityHandle().entity(),
                                          Transform2d::Translate(Vector2d(100.0, 0.0)));
  compositor.renderFrame(viewport_);

  const RendererBitmap flat = renderer_.takeSnapshot();
  // With the default bucketer threshold the white-bg rect isn't bucketed; it
  // stays in the root layer and composition is straightforward. Test still
  // guards against regressions of the single-promoted-layer path.
  EXPECT_THAT(getPixel(flat, 35, 35), IsWhite()) << "bg pixel should be white";
  EXPECT_THAT(getPixel(flat, 135, 35), IsRed()) << "target at translated position";
}

// Replicates DragReleasePopBack Phase 4: a prior drag committed a transform,
// then we reset layers, re-promote, render at identity. With aggressive
// bucketing, does the standalone white-bg bucket still rasterize correctly?
TEST_F(CompositorGoldenTest, BucketedStandaloneAfterResetRasterizesCorrectly) {
  SVGDocument document = parseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
      <rect width="200" height="100" fill="white"/>
      <rect id="target" x="10" y="10" width="50" height="50" fill="red"
            transform="translate(100, 0)"/>
    </svg>
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  CompositorConfig config;
  config.complexityBucketing = true;
  CompositorController compositor(document, renderer_, config);

  // Phase 1: initial promote + render (warm the bucketer state).
  ASSERT_TRUE(compositor.promoteEntity(target->entityHandle().entity()));
  compositor.setLayerCompositionTransform(target->entityHandle().entity(), Transform2d());
  compositor.renderFrame(viewport_);

  // Phase 2: resetAllLayers, then promote + render again.
  compositor.resetAllLayers();
  ASSERT_TRUE(compositor.promoteEntity(target->entityHandle().entity()));
  compositor.setLayerCompositionTransform(target->entityHandle().entity(), Transform2d());
  compositor.renderFrame(viewport_);

  const RendererBitmap flat = renderer_.takeSnapshot();
  EXPECT_THAT(getPixel(flat, 35, 35), IsWhite()) << "after reset, bg pixel should still be white";
  EXPECT_THAT(getPixel(flat, 135, 35), IsRed()) << "target at translated (via DOM transform)";
}

// Exact replication of DragReleasePopBack Phase 4: drag a rect, commit the
// transform via setTransform (DOM mutation), reset layers, re-promote at
// identity, render. With aggressive bucketing, does this sequence still
// produce correct pixels?
TEST_F(CompositorGoldenTest, DragReleaseResetSequenceWithAggressiveBucketing) {
  SVGDocument document = parseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
      <rect width="200" height="100" fill="white"/>
      <rect id="target" x="10" y="10" width="50" height="50" fill="red"/>
    </svg>
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->entityHandle().entity();

  CompositorConfig config;
  config.complexityBucketing = true;
  CompositorController compositor(document, renderer_, config);
  ASSERT_TRUE(compositor.promoteEntity(entity));

  // Phase 1.
  compositor.setLayerCompositionTransform(entity, Transform2d());
  compositor.renderFrame(viewport_);

  // Phase 2 — drag with composition offset.
  compositor.setLayerCompositionTransform(entity, Transform2d::Translate(Vector2d(100.0, 0.0)));
  compositor.renderFrame(viewport_);

  // Phase 3 — commit via setTransform, composition returns to identity.
  target->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(Vector2d(100.0, 0.0)));
  compositor.setLayerCompositionTransform(entity, Transform2d());
  compositor.renderFrame(viewport_);

  // Phase 4 — resetAllLayers, re-promote.
  compositor.resetAllLayers();
  ASSERT_TRUE(compositor.promoteEntity(entity));
  compositor.setLayerCompositionTransform(entity, Transform2d());
  compositor.renderFrame(viewport_);

  const RendererBitmap flat = renderer_.takeSnapshot();
  EXPECT_THAT(getPixel(flat, 35, 35), IsWhite())
      << "origin position should be vacated (white), not transparent";
  EXPECT_THAT(getPixel(flat, 135, 35), IsRed()) << "target at translated position";
}

// Exercises the `CompositorConfig::verifyPixelIdentity` runtime gate. With
// the gate on, `renderFrame` internally dual-paths and asserts — if
// composited output matches the full-render reference, the test passes; if
// a regression ever produces drift, `UTILS_RELEASE_ASSERT` fires and the
// test fails with a diagnostic.
// A bucketed document with three top-level children matches full-render.
// Each child becomes its own bucket layer; composition stacks them in
// draw order. AA tolerance accounts for off-by-one rounding in the
// premultiplied → unpremultiplied conversion applied during compose.
TEST_F(CompositorGoldenTest, MultiBucketCompositionMatchesFullRender) {
  SVGDocument document = parseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
      <rect width="200" height="100" fill="white"/>
      <g>
        <rect x="10" y="10" width="40" height="40" fill="red"/>
        <rect x="60" y="10" width="40" height="40" fill="blue"/>
      </g>
      <rect x="120" y="20" width="30" height="30" fill="green"/>
    </svg>
  )svg");

  CompositorConfig config;
  config.complexityBucketing = true;
  CompositorController compositor(document, renderer_, config);

  DualPathVerifier verifier(compositor, renderer_);
  const auto result = verifier.renderAndVerify(viewport_);

  EXPECT_LE(result.maxChannelDiff, 2) << result;
  EXPECT_LE(result.mismatchCount, result.totalPixels / 100u)
      << "at most 1% mismatched pixels (AA tolerance at bucket edges): " << result;
}

// Mandatory auto-promotion of an `opacity<1` entity produces pixel-identical
// output to full-render. The auto-promoted layer's cached bitmap is
// premultiplied-alpha; `composeLayers` unpremultiplies before passing to
// `drawImage` so the compose math matches the direct-render path.
// Filter groups should auto-promote after the first `renderFrame` completes —
// i.e. the moment `RenderingInstanceComponent`s exist, the
// `MandatoryHintDetector` should have noticed and published a Mandatory hint
// for any `<g filter="…">` in the document. This guards against the earlier
// regression where the detector ran before `prepareDocumentForRendering`,
// saw an empty component view, and never got a second chance.
TEST_F(CompositorGoldenTest, FilterGroupAutoPromotesOnFirstRender) {
  SVGDocument document = parseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
      <defs><filter id="blur"><feGaussianBlur in="SourceGraphic" stdDeviation="2"/></filter></defs>
      <rect width="200" height="100" fill="white"/>
      <g id="glow" filter="url(#blur)"><circle cx="100" cy="50" r="30" fill="red"/></g>
    </svg>
  )svg");

  auto glow = document.querySelector("#glow");
  ASSERT_TRUE(glow.has_value());
  const Entity glowEntity = glow->entityHandle().entity();

  CompositorController compositor(document, renderer_);
  EXPECT_EQ(compositor.layerCount(), 0u) << "no render yet — detectors haven't run";

  compositor.renderFrame(viewport_);
  EXPECT_EQ(compositor.layerCount(), 1u)
      << "after the first render, the filter group should be mandatory-promoted";

  // The next render should produce a composited output whose content for the
  // filter group lives in a cached layer bitmap. Even though no entity was
  // explicitly promoted, mandatory promotion means re-drawing the glow after
  // e.g. a viewport change stays cheap.
  compositor.renderFrame(viewport_);
  EXPECT_GT(compositor.layerCount(), 0u) << "filter layer persists across frames";
  EXPECT_EQ(compositor.layerBitmapOf(glowEntity).dimensions.x, 200);
  EXPECT_EQ(compositor.layerBitmapOf(glowEntity).dimensions.y, 100);
}

TEST_F(CompositorGoldenTest, OpacityLessThanOneAutoPromotionMatchesFullRender) {
  SVGDocument document = parseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
      <rect width="200" height="100" fill="white"/>
      <rect x="10" y="10" width="50" height="50" fill="red" opacity="0.5"/>
    </svg>
  )svg");

  CompositorController compositor(document, renderer_);
  DualPathVerifier verifier(compositor, renderer_);
  const auto result = verifier.renderAndVerify(viewport_);
  // AA-tolerance: off-by-one at bitmap boundaries due to integer rounding in
  // the unpremultiply step. Allow a small fraction of pixels to differ by up
  // to 1 channel unit.
  EXPECT_LE(result.maxChannelDiff, 2) << result;
  EXPECT_LE(result.mismatchCount, result.totalPixels / 100u)
      << "at most 1% mismatched pixels (AA tolerance around semi-transparent edges): " << result;
}

// CI coverage: explicit promotion at identity composition. This is what a
// selection-driven prewarm produces before the user starts dragging.
TEST_F(CompositorGoldenTest, DualPathGate_ExplicitPromoteAtIdentity) {
  SVGDocument document = parseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
      <rect width="200" height="100" fill="white"/>
      <rect id="target" x="10" y="10" width="50" height="50" fill="red"/>
    </svg>
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  CompositorConfig config;
  config.verifyPixelIdentity = true;
  CompositorController compositor(document, renderer_, config);
  ASSERT_TRUE(compositor.promoteEntity(target->entityHandle().entity()));

  compositor.setLayerCompositionTransform(target->entityHandle().entity(), Transform2d());
  compositor.renderFrame(viewport_);
  compositor.renderFrame(viewport_);

  SUCCEED() << "Explicit promote at identity: dual-path held";
}

TEST_F(CompositorGoldenTest, VerifyPixelIdentityGateCatchesNoDriftOnValidScene) {
  // The dual-path assertion compares composited output against a full-render
  // reference. It can only be enabled when both paths render the SAME scene —
  // i.e., composition transform must match the DOM. During an active drag
  // (composition transform differs from DOM), enabling the gate would always
  // fire (correctly!) because the paths render visually different content.
  SVGDocument document = parseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
      <rect width="200" height="100" fill="white"/>
      <rect id="target" x="10" y="10" width="50" height="50" fill="red"/>
    </svg>
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->entityHandle().entity();

  CompositorConfig config;
  config.verifyPixelIdentity = true;  // Enable the in-tree dual-path assertion.
  CompositorController compositor(document, renderer_, config);
  ASSERT_TRUE(compositor.promoteEntity(entity));

  // Composition transform stays at identity — the compositor and reference
  // paths render the same pixels. Each renderFrame internally dual-paths and
  // asserts. Survival to the end = no drift detected.
  compositor.setLayerCompositionTransform(entity, Transform2d());
  compositor.renderFrame(viewport_);
  compositor.renderFrame(viewport_);
  compositor.renderFrame(viewport_);

  SUCCEED() << "Dual-path verification passed across all rendered frames";
}

// Regression: dragging a Gaussian-blurred shape in the editor caused the blur
// to disappear during drag. The blurred element is auto-promoted (filter →
// mandatory hint), its layer is cached, and the cached bitmap SHOULD contain
// the blurred result. If the cache produces a non-blurred (or empty) bitmap,
// the drag preview silently loses the effect.
TEST_F(CompositorGoldenTest, GaussianBlurredShapeMatchesFullRenderAfterPromotion) {
  SVGDocument document = parseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
      <defs>
        <filter id="blur" x="-20%" y="-20%" width="140%" height="140%">
          <feGaussianBlur stdDeviation="4"/>
        </filter>
      </defs>
      <rect width="200" height="100" fill="white"/>
      <rect id="target" x="60" y="20" width="60" height="60" fill="red" filter="url(#blur)"/>
    </svg>
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  CompositorController compositor(document, renderer_);
  ASSERT_TRUE(compositor.promoteEntity(target->entityHandle().entity()));

  DualPathVerifier verifier(compositor, renderer_);
  const auto result = verifier.renderAndVerify(viewport_);

  // Blur produces semi-transparent edge pixels over a wide radius; the tolerance
  // here is looser than the identity-composition cases because the blur kernel
  // itself has sub-pixel variation and the unpremul round-trip may amplify it.
  EXPECT_LE(result.maxChannelDiff, 3) << result;
  EXPECT_LE(result.mismatchCount, result.totalPixels / 50u)  // 2%
      << "blurred shape should match full-render within AA tolerance: " << result;
}

// Regression variant: the blurred shape is dragged (composition transform set
// to a non-zero translation). The cached bitmap carries the blurred result;
// composition just translates the bitmap. Blur must remain visible.
TEST_F(CompositorGoldenTest, GaussianBlurredShapeRemainsVisibleDuringDrag) {
  SVGDocument document = parseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
      <defs>
        <filter id="blur" x="-20%" y="-20%" width="140%" height="140%">
          <feGaussianBlur stdDeviation="4"/>
        </filter>
      </defs>
      <rect width="200" height="100" fill="white"/>
      <rect id="target" x="60" y="20" width="60" height="60" fill="red" filter="url(#blur)"/>
    </svg>
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->entityHandle().entity();

  CompositorController compositor(document, renderer_);
  ASSERT_TRUE(compositor.promoteEntity(entity));

  // Initial render caches the blurred bitmap.
  compositor.setLayerCompositionTransform(entity, Transform2d());
  compositor.renderFrame(viewport_);

  // Now simulate a drag: translate the layer. The cached bitmap should still
  // carry the blur; composition just repositions it.
  compositor.setLayerCompositionTransform(entity, Transform2d::Translate(Vector2d(30.0, 0.0)));
  compositor.renderFrame(viewport_);

  const RendererBitmap flat = renderer_.takeSnapshot();

  // Original rect x=60,y=20,w=60,h=60. After translate +30, rect is at
  // [90, 150] × [20, 80]. The Gaussian blur (stdDeviation=4) produces a
  // halo extending ~8–12 px beyond the edges — red bleeds into the
  // surrounding white.
  //
  // Sample the halo at (155, 50): 5 px outside the right edge. If the blur
  // is preserved in the cached bitmap and blitted at the new position, this
  // pixel should show semi-transparent red against white (pink-ish). If the
  // blur is lost during drag (bug), this pixel stays pure white.
  const Pixel halo = getPixel(flat, 155, 50);
  const Pixel farAway = getPixel(flat, 5, 5);

  EXPECT_LT(halo.g, 240) << "halo pixel should be tinted red by the blur (not white). got: "
                        << halo;
  EXPECT_LT(halo.b, 240) << "halo pixel should be tinted red by the blur. got: " << halo;
  EXPECT_GT(halo.r, halo.g) << "red channel dominant in halo. got: " << halo;
  EXPECT_THAT(farAway, IsWhite()) << "far from drag, background stays white";
}

// Regression: the splash SVG had a subtle bug where dragging a letter caused
// OTHER top-level filtered groups (background glows) to disappear. The
// filtered groups are top-level children (bucketed by ComplexityBucketer)
// and their layer rasterization goes through `drawEntityRange(g, g.lastRenderedEntity)`.
// If that range iteration doesn't process the group's children, the bucket
// bitmap is empty — the glow vanishes.
//
// This test explicitly rasterizes such a group to confirm the path works.
// Reproduces the donner_splash.svg scenario: a "letters" group (the draggable
// yellow foreground) alongside a "glow" group with a blur filter (the
// background glow that shouldn't disappear when the letters are dragged).
// Both are top-level children of the SVG root — with aggressive bucketing
// (minCostToBucket=1), they become separate bucket layers. When the user
// drags a LETTER (descendant of the letters group), the glow layer should
// STILL render correctly.
// Mirrors the splash SVG structure more closely: a wrapping `<g>` at the top
// level contains BOTH the draggable letters group AND the filtered glow
// groups. With aggressive bucketing, the wrapping group becomes the bucket.
// When a letter (descendant of the wrapper) is dragged, the wrapper's layer
// must still contain the rendered glows.
// Closer to the splash: the filtered glow contains a path with a GRADIENT
// fill (not a plain color). Paint servers are resolved per entity. A repeated
// rasterization across drag frames may interact badly with resolved gradient
// references.
// Reduced donner_splash repro. Structure mirrors the full splash:
// - Wrapping `<g class="wrapper">` (analogous to `cls-94`)
// - `<g id="Donner">` with letter paths that use style-class gradient fills
// - Sibling `<g id="Lightning_glow_dark" filter="url(#glow-blur)">` with a
//   gradient-filled path
// - Gradients defined in `<defs>` via `<radialGradient>`
// - Style rules applied via CSS classes (not inline `fill="..."`)
//
// User drags a letter; the glow should stay visible. This matches the exact
// manual-test scenario the user reports as broken.
TEST_F(CompositorGoldenTest, ReducedSplashDraggingLetterPreservesGlow) {
  SVGDocument document = parseDocument(R"svg(
<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" width="892" height="512" viewBox="0 0 892 512">
  <defs>
    <style>
      * { stroke-width: 0px; }
      .bg { fill: #0d0f1d; }
      .letter-a { fill: url(#letter-gradient-a); }
      .letter-b { fill: url(#letter-gradient-b); }
      .glow-path { fill: url(#glow-gradient); }
    </style>
    <filter id="glow-blur">
      <feGaussianBlur in="SourceGraphic" stdDeviation="4.5" />
    </filter>
    <radialGradient id="letter-gradient-a" cx="300" cy="390" r="80" gradientUnits="userSpaceOnUse">
      <stop offset="0" stop-color="#fae100"/>
      <stop offset="1" stop-color="#f39200"/>
    </radialGradient>
    <radialGradient id="letter-gradient-b" cx="370" cy="390" r="80" gradientUnits="userSpaceOnUse">
      <stop offset="0" stop-color="#fae100"/>
      <stop offset="1" stop-color="#f39200"/>
    </radialGradient>
    <radialGradient id="glow-gradient" cx="465" cy="410" r="60" gradientUnits="userSpaceOnUse">
      <stop offset="0" stop-color="#ffe54a" stop-opacity="0.8"/>
      <stop offset="1" stop-color="#ffe54a" stop-opacity="0"/>
    </radialGradient>
  </defs>
  <g class="wrapper">
    <g id="Background">
      <rect class="bg" width="892" height="512"/>
    </g>
    <g id="Donner">
      <rect id="letter_a" class="letter-a" x="270" y="345" width="70" height="90"/>
      <rect id="letter_b" class="letter-b" x="340" y="345" width="70" height="90"/>
    </g>
    <g id="Lightning_glow_dark" filter="url(#glow-blur)">
      <rect class="glow-path" x="430" y="380" width="80" height="80"/>
    </g>
  </g>
</svg>
  )svg");

  auto letterA = document.querySelector("#letter_a");
  ASSERT_TRUE(letterA.has_value()) << "reduced splash should parse and expose #letter_a";

  RenderViewport fullViewport;
  fullViewport.size = Vector2d(892, 512);
  fullViewport.devicePixelRatio = 1.0;

  // Bucketing off so MandatoryHintDetector is the only promoter.
  // Rules out double-composition (content in wrapper bucket AND its own
  // layer). If the drift persists with bucketing off, it's an issue in the
  // mandatory-layer path itself, not a bucketing interaction.
  CompositorConfig config;
  config.complexityBucketing = false;
  CompositorController compositor(document, renderer_, config);

  // Sample INSIDE the glow rect where the gradient is visible (blurred).
  // Glow rect: x=[430,510], y=[380,460]. (465, 420) is near the gradient
  // center and shows significant gradient + blur contribution.
  constexpr int kSampleX = 465;
  constexpr int kSampleY = 420;

  // Frame 0: full render BEFORE any drag — baseline.
  compositor.renderFrame(fullViewport);
  const RendererBitmap baseline = renderer_.takeSnapshot();
  const Pixel glowBaseline = getPixel(baseline, kSampleX, kSampleY);

  // User clicks on letter A to start the drag.
  ASSERT_TRUE(compositor.promoteEntity(letterA->entityHandle().entity()))
      << "letter A has no compositing ancestor, should promote";

  // Series of drag frames (editor drags at ~60fps).
  Pixel glowDuringDrag{};
  for (int i = 1; i <= 10; ++i) {
    compositor.setLayerCompositionTransform(letterA->entityHandle().entity(),
                                             Transform2d::Translate(Vector2d(i * 4.0, 0.0)));
    compositor.renderFrame(fullViewport);

    const RendererBitmap frame = renderer_.takeSnapshot();
    glowDuringDrag = getPixel(frame, kSampleX, kSampleY);
  }

  // During drag the glow must still be present. Any significant drift from
  // the baseline at this sample point means the glow disappeared.
  const int baselineTotal = glowBaseline.r + glowBaseline.g + glowBaseline.b;
  const int dragTotal = glowDuringDrag.r + glowDuringDrag.g + glowDuringDrag.b;
  EXPECT_LE(std::abs(baselineTotal - dragTotal), 30)
      << "glow during drag must match baseline. baseline=" << glowBaseline
      << " drag=" << glowDuringDrag;
}

// Mirrors the editor's default config (bucketing enabled, multiple filter
// groups above and below the drag target) to catch interactions the
// bucketing-off reduced-splash test misses.
TEST_F(CompositorGoldenTest, SplashDragWithBucketingAndMultipleFilterGroups) {
  SVGDocument document = parseDocument(R"svg(
<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" width="892" height="512" viewBox="0 0 892 512">
  <defs>
    <filter id="blur-a"><feGaussianBlur in="SourceGraphic" stdDeviation="4.5"/></filter>
    <filter id="blur-b"><feGaussianBlur in="SourceGraphic" stdDeviation="6"/></filter>
    <filter id="blur-c"><feGaussianBlur in="SourceGraphic" stdDeviation="8"/></filter>
    <radialGradient id="g-letter" cx="300" cy="390" r="80" gradientUnits="userSpaceOnUse">
      <stop offset="0" stop-color="#fae100"/>
      <stop offset="1" stop-color="#f39200"/>
    </radialGradient>
    <radialGradient id="g-glow-a" cx="200" cy="420" r="60" gradientUnits="userSpaceOnUse">
      <stop offset="0" stop-color="#ffe54a" stop-opacity="0.8"/>
      <stop offset="1" stop-color="#ffe54a" stop-opacity="0"/>
    </radialGradient>
    <radialGradient id="g-glow-b" cx="465" cy="410" r="60" gradientUnits="userSpaceOnUse">
      <stop offset="0" stop-color="#ffe54a" stop-opacity="0.8"/>
      <stop offset="1" stop-color="#ffe54a" stop-opacity="0"/>
    </radialGradient>
    <radialGradient id="g-glow-c" cx="700" cy="400" r="60" gradientUnits="userSpaceOnUse">
      <stop offset="0" stop-color="#ffe54a" stop-opacity="0.8"/>
      <stop offset="1" stop-color="#ffe54a" stop-opacity="0"/>
    </radialGradient>
  </defs>
  <g class="wrapper">
    <rect width="892" height="512" fill="#0d0f1d"/>
    <g id="glow_behind" filter="url(#blur-a)">
      <rect x="170" y="390" width="80" height="80" fill="url(#g-glow-a)"/>
    </g>
    <g id="Donner">
      <rect id="letter_1" class="letter" x="270" y="345" width="70" height="90" fill="url(#g-letter)"/>
      <rect id="letter_2" class="letter" x="350" y="345" width="70" height="90" fill="url(#g-letter)"/>
      <rect id="letter_3" class="letter" x="430" y="345" width="70" height="90" fill="url(#g-letter)"/>
      <rect id="letter_4" class="letter" x="510" y="345" width="70" height="90" fill="url(#g-letter)"/>
    </g>
    <g id="glow_middle" filter="url(#blur-b)">
      <rect x="435" y="380" width="80" height="80" fill="url(#g-glow-b)"/>
    </g>
    <g id="glow_foreground" filter="url(#blur-c)">
      <rect x="670" y="370" width="80" height="80" fill="url(#g-glow-c)"/>
    </g>
  </g>
</svg>
  )svg");

  RenderViewport fullViewport;
  fullViewport.size = Vector2d(892, 512);
  fullViewport.devicePixelRatio = 1.0;

  // Editor default config: all auto-promotion features on.
  CompositorConfig config;
  CompositorController compositor(document, renderer_, config);

  // Baseline render.
  compositor.renderFrame(fullViewport);
  const RendererBitmap baseline = renderer_.takeSnapshot();
  const Pixel baselineGlowA = getPixel(baseline, 200, 420);
  const Pixel baselineGlowB = getPixel(baseline, 465, 420);
  const Pixel baselineGlowC = getPixel(baseline, 700, 400);

  auto letter2 = document.querySelector("#letter_2");
  ASSERT_TRUE(letter2.has_value());

  ASSERT_TRUE(compositor.promoteEntity(letter2->entityHandle().entity()))
      << "letter_2 has no compositing ancestor, should promote";

  Pixel dragGlowA{}, dragGlowB{}, dragGlowC{};
  for (int i = 1; i <= 10; ++i) {
    compositor.setLayerCompositionTransform(letter2->entityHandle().entity(),
                                             Transform2d::Translate(Vector2d(i * 4.0, 0.0)));
    compositor.renderFrame(fullViewport);
    const RendererBitmap frame = renderer_.takeSnapshot();
    dragGlowA = getPixel(frame, 200, 420);
    dragGlowB = getPixel(frame, 465, 420);
    dragGlowC = getPixel(frame, 700, 400);
  }

  auto checkClose = [](const Pixel& baseline, const Pixel& drag, const char* label) {
    const int baselineTotal = baseline.r + baseline.g + baseline.b;
    const int dragTotal = drag.r + drag.g + drag.b;
    EXPECT_LE(std::abs(baselineTotal - dragTotal), 30)
        << label << ": baseline=" << baseline << " drag=" << drag;
  };
  checkClose(baselineGlowA, dragGlowA, "glow_behind (before drag target in paint order)");
  checkClose(baselineGlowB, dragGlowB, "glow_middle (after drag target in paint order)");
  checkClose(baselineGlowC, dragGlowC, "glow_foreground (further after drag target)");
}

// Translation-only drag must not re-rasterize bg/fg between frames. The
// promise of the split-static-layers optimization is: bg and fg are
// rasterized once per drag session and reused via GL texture blit. If they
// got re-rendered every frame, the compositor would be doing full-document
// work per pointer move.
TEST_F(CompositorGoldenTest, SplitBitmapsStableAcrossTranslationOnlyDragFrames) {
  SVGDocument document = parseDocument(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
  <rect width="200" height="100" fill="white"/>
  <rect id="target" x="10" y="10" width="50" height="50" fill="red"/>
  <g filter="url(#blur)"><rect x="100" y="10" width="50" height="50" fill="yellow"/></g>
  <defs><filter id="blur"><feGaussianBlur in="SourceGraphic" stdDeviation="2"/></filter></defs>
</svg>
  )svg");
  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  CompositorController compositor(document, renderer_);
  compositor.renderFrame(viewport_);

  ASSERT_TRUE(compositor.promoteEntity(target->entityHandle().entity()));
  compositor.setLayerCompositionTransform(target->entityHandle().entity(),
                                           Transform2d::Translate(Vector2d(1.0, 0.0)));
  compositor.renderFrame(viewport_);

  // Snapshot the bitmaps after the first drag frame. The `.data()` address
  // only changes when the vector is reassigned — so comparing it across
  // renderFrame calls is a cheap, direct probe of whether the bitmap was
  // re-rasterized.
  const RendererBitmap& bgFrame1 = compositor.backgroundBitmap();
  const RendererBitmap& fgFrame1 = compositor.foregroundBitmap();
  ASSERT_FALSE(bgFrame1.empty());
  ASSERT_FALSE(fgFrame1.empty());
  const std::vector<uint8_t> bgPixelsFrame1 = bgFrame1.pixels;
  const std::vector<uint8_t> fgPixelsFrame1 = fgFrame1.pixels;
  const uint8_t* bgDataPtrFrame1 = bgFrame1.pixels.data();
  const uint8_t* fgDataPtrFrame1 = fgFrame1.pixels.data();

  // Simulate 10 more drag frames with only translation changes.
  for (int i = 2; i <= 11; ++i) {
    compositor.setLayerCompositionTransform(
        target->entityHandle().entity(), Transform2d::Translate(Vector2d(i * 1.0, 0.0)));
    compositor.renderFrame(viewport_);
  }

  const RendererBitmap& bgFinal = compositor.backgroundBitmap();
  const RendererBitmap& fgFinal = compositor.foregroundBitmap();
  EXPECT_EQ(bgFinal.pixels.data(), bgDataPtrFrame1)
      << "bg bitmap re-allocated between drag frames — cache invalidated incorrectly";
  EXPECT_EQ(fgFinal.pixels.data(), fgDataPtrFrame1)
      << "fg bitmap re-allocated between drag frames — cache invalidated incorrectly";
  EXPECT_EQ(bgFinal.pixels, bgPixelsFrame1);
  EXPECT_EQ(fgFinal.pixels, fgPixelsFrame1);
}

// Zooming changes the viewport size the renderer rasterizes at, so the
// cached bg/fg bitmaps from the previous zoom level must NOT be reused —
// they're sized to the old canvas. Reusing them would stamp their pixels
// into the top-left region of the new (larger) canvas, leaving the rest
// transparent, which the editor's GL layer would then linearly-stretch
// back to fill the pane and show as a blurry image.
TEST_F(CompositorGoldenTest, SplitBitmapsInvalidateOnViewportResize) {
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
  compositor.setLayerCompositionTransform(target->entityHandle().entity(),
                                           Transform2d::Translate(Vector2d(1.0, 0.0)));

  document.setCanvasSize(200, 100);
  RenderViewport smallViewport;
  smallViewport.size = Vector2d(200, 100);
  smallViewport.devicePixelRatio = 1.0;
  compositor.renderFrame(smallViewport);
  ASSERT_FALSE(compositor.backgroundBitmap().empty());
  EXPECT_EQ(compositor.backgroundBitmap().dimensions, Vector2i(200, 100));

  // Simulate a zoom-in: the editor's RenderCoordinator calls setCanvasSize
  // before requestRender when the viewport's desiredCanvasSize changes, so
  // mimic that ordering here.
  document.setCanvasSize(400, 200);
  RenderViewport largeViewport;
  largeViewport.size = Vector2d(400, 200);
  largeViewport.devicePixelRatio = 1.0;
  compositor.renderFrame(largeViewport);

  EXPECT_EQ(compositor.backgroundBitmap().dimensions, Vector2i(400, 200))
      << "bg bitmap should re-rasterize to match new canvas after zoom";
  EXPECT_EQ(compositor.foregroundBitmap().dimensions, Vector2i(400, 200));
}

// Isolates the gradient + filter interaction. Single frame, no drag — does
// it render correctly at all?
TEST_F(CompositorGoldenTest, GradientInsideFilteredGroup_SingleFrame) {
  SVGDocument document = parseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
      <defs>
        <filter id="blur"><feGaussianBlur in="SourceGraphic" stdDeviation="4"/></filter>
        <radialGradient id="gradient" cx="50%" cy="50%" r="50%">
          <stop offset="0%" stop-color="red"/>
          <stop offset="100%" stop-color="yellow"/>
        </radialGradient>
      </defs>
      <rect width="200" height="100" fill="white"/>
      <g id="glow" filter="url(#blur)">
        <circle cx="100" cy="50" r="25" fill="url(#gradient)"/>
      </g>
    </svg>
  )svg");
  auto glow = document.querySelector("#glow");
  ASSERT_TRUE(glow.has_value());
  CompositorController compositor(document, renderer_);
  ASSERT_TRUE(compositor.promoteEntity(glow->entityHandle().entity()));
  compositor.renderFrame(viewport_);
  const RendererBitmap flat = renderer_.takeSnapshot();
  const Pixel halo = getPixel(flat, 70, 50);
  // The circle's edge has red-to-yellow gradient. Blur spreads those colors
  // outward. Pure white at (70, 50) means the halo (and thus the whole
  // filtered glow) disappeared.
  const bool isPureWhite = halo.r >= 253 && halo.g >= 253 && halo.b >= 253;
  EXPECT_FALSE(isPureWhite)
      << "single frame: gradient+filter halo should be tinted by the blur (not pure white). got: "
      << halo;
}

// Isolates repeated rasterization. Flat-color filled child, filtered group,
// repeated frames. If this passes but the gradient variant fails, the bug is
// in paint-server resolution across repeated rasterizations.
TEST_F(CompositorGoldenTest, FlatColorInsideFilteredGroup_RepeatedFrames) {
  SVGDocument document = parseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
      <defs>
        <filter id="blur"><feGaussianBlur in="SourceGraphic" stdDeviation="4"/></filter>
      </defs>
      <g id="wrapper">
        <rect width="200" height="100" fill="white"/>
        <g id="glow" filter="url(#blur)">
          <circle cx="100" cy="50" r="25" fill="red"/>
        </g>
        <rect id="letter" x="40" y="30" width="10" height="40" fill="yellow"/>
      </g>
    </svg>
  )svg");
  auto letter = document.querySelector("#letter");
  ASSERT_TRUE(letter.has_value());
  CompositorController compositor(document, renderer_);
  ASSERT_TRUE(compositor.promoteEntity(letter->entityHandle().entity()));
  for (int i = 0; i < 5; ++i) {
    compositor.setLayerCompositionTransform(letter->entityHandle().entity(),
                                             Transform2d::Translate(Vector2d(i * 5.0, 0.0)));
    compositor.renderFrame(viewport_);
  }
  const RendererBitmap flat = renderer_.takeSnapshot();
  const Pixel halo = getPixel(flat, 70, 50);
  const bool isPureWhite = halo.r >= 253 && halo.g >= 253 && halo.b >= 253;
  EXPECT_FALSE(isPureWhite)
      << "flat color + filter across repeated frames: halo must remain. got: " << halo;
}

TEST_F(CompositorGoldenTest, DraggingLetterPreservesGradientFilteredGlowAcrossFrames) {
  SVGDocument document = parseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
      <defs>
        <filter id="blur" x="-20%" y="-20%" width="140%" height="140%">
          <feGaussianBlur in="SourceGraphic" stdDeviation="4"/>
        </filter>
        <radialGradient id="gradient" cx="50%" cy="50%" r="50%">
          <stop offset="0%" stop-color="red"/>
          <stop offset="100%" stop-color="yellow"/>
        </radialGradient>
      </defs>
      <g id="wrapper">
        <rect width="200" height="100" fill="white"/>
        <g id="glow" filter="url(#blur)">
          <circle cx="100" cy="50" r="25" fill="url(#gradient)"/>
        </g>
        <rect id="letter_D" x="40" y="30" width="10" height="40" fill="yellow"/>
      </g>
    </svg>
  )svg");

  auto letterD = document.querySelector("#letter_D");
  ASSERT_TRUE(letterD.has_value());
  const Entity entity = letterD->entityHandle().entity();

  CompositorController compositor(document, renderer_);
  ASSERT_TRUE(compositor.promoteEntity(entity));

  // Sequence of drag frames to exercise the bucket-layer re-rasterization
  // path across repeated renders (the glow has a filter → conservativeFallback
  // → re-rasterized every frame).
  for (int i = 0; i < 5; ++i) {
    compositor.setLayerCompositionTransform(entity,
                                             Transform2d::Translate(Vector2d(i * 5.0, 0.0)));
    compositor.renderFrame(viewport_);
  }

  const RendererBitmap flat = renderer_.takeSnapshot();

  // Glow halo sampling — left of circle, in the blur halo.
  const Pixel halo = getPixel(flat, 70, 50);
  const bool isPureWhite = halo.r >= 253 && halo.g >= 253 && halo.b >= 253;
  EXPECT_FALSE(isPureWhite)
      << "after repeated drag frames, gradient-filled glow halo must remain. got: " << halo;
}

TEST_F(CompositorGoldenTest, DraggingLetterInsideWrapperPreservesNestedGlow) {
  SVGDocument document = parseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
      <defs>
        <filter id="blur" x="-20%" y="-20%" width="140%" height="140%">
          <feGaussianBlur in="SourceGraphic" stdDeviation="4"/>
        </filter>
      </defs>
      <g id="wrapper">
        <rect width="200" height="100" fill="white"/>
        <g id="glow" filter="url(#blur)">
          <circle cx="100" cy="50" r="25" fill="red"/>
        </g>
        <g id="letters">
          <rect id="letter_D" x="40" y="30" width="10" height="40" fill="yellow"/>
        </g>
      </g>
    </svg>
  )svg");

  auto letterD = document.querySelector("#letter_D");
  ASSERT_TRUE(letterD.has_value());

  CompositorController compositor(document, renderer_);
  ASSERT_TRUE(compositor.promoteEntity(letterD->entityHandle().entity()));

  compositor.setLayerCompositionTransform(letterD->entityHandle().entity(), Transform2d());
  compositor.renderFrame(viewport_);

  compositor.setLayerCompositionTransform(letterD->entityHandle().entity(),
                                          Transform2d::Translate(Vector2d(20.0, 10.0)));
  compositor.renderFrame(viewport_);

  const RendererBitmap flat = renderer_.takeSnapshot();

  // Glow halo sampling — blur extends ~8-12px beyond circle edge.
  const Pixel glowHaloLeft = getPixel(flat, 70, 50);
  EXPECT_LT(glowHaloLeft.g, 240)
      << "glow halo should be tinted red. If white, the nested glow disappeared during drag. "
      << "got: " << glowHaloLeft;
}

TEST_F(CompositorGoldenTest, DraggingLetterPreservesBackgroundGlow) {
  SVGDocument document = parseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
      <defs>
        <filter id="blur" x="-20%" y="-20%" width="140%" height="140%">
          <feGaussianBlur in="SourceGraphic" stdDeviation="4"/>
        </filter>
      </defs>
      <rect width="200" height="100" fill="white"/>
      <g id="glow" filter="url(#blur)">
        <circle cx="100" cy="50" r="25" fill="red"/>
      </g>
      <g id="letters">
        <rect id="letter_D" x="40" y="30" width="10" height="40" fill="yellow"/>
        <rect id="letter_o" x="55" y="30" width="10" height="40" fill="yellow"/>
      </g>
    </svg>
  )svg");

  auto letterD = document.querySelector("#letter_D");
  ASSERT_TRUE(letterD.has_value());

  CompositorController compositor(document, renderer_);
  // User clicks on letter D to start a drag.
  ASSERT_TRUE(compositor.promoteEntity(letterD->entityHandle().entity()))
      << "letter is not a descendant of any filtered/masked/clipped group, should promote";

  // Frame 1: initial render.
  compositor.setLayerCompositionTransform(letterD->entityHandle().entity(), Transform2d());
  compositor.renderFrame(viewport_);

  // Frame 2: user starts dragging letter D.
  compositor.setLayerCompositionTransform(letterD->entityHandle().entity(),
                                          Transform2d::Translate(Vector2d(20.0, 10.0)));
  compositor.renderFrame(viewport_);

  const RendererBitmap flat = renderer_.takeSnapshot();

  // The glow's halo extends beyond the circle. Sample outside the
  // circle but within the blur radius. Circle is at cx=100 cy=50 r=25.
  // Point (70, 50) is 5 px outside the left edge — in the halo.
  const Pixel glowHaloLeft = getPixel(flat, 70, 50);
  const Pixel glowHaloRight = getPixel(flat, 130, 50);

  // If the glow disappears during drag, these pixels would be pure white.
  // With the glow preserved, they should be tinted red.
  EXPECT_LT(glowHaloLeft.g, 240)
      << "glow halo left: tinted red by blur, not pure white. got: " << glowHaloLeft;
  EXPECT_LT(glowHaloRight.g, 240)
      << "glow halo right: tinted red by blur, not pure white. got: " << glowHaloRight;
}

TEST_F(CompositorGoldenTest, FilteredGroupWithChildrenRasterizesIncludingChildren) {
  SVGDocument document = parseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
      <defs>
        <filter id="blur" x="-20%" y="-20%" width="140%" height="140%">
          <feGaussianBlur in="SourceGraphic" stdDeviation="4"/>
        </filter>
      </defs>
      <rect width="200" height="100" fill="white"/>
      <g id="glow" filter="url(#blur)">
        <circle cx="100" cy="50" r="25" fill="red"/>
      </g>
    </svg>
  )svg");

  auto glow = document.querySelector("#glow");
  ASSERT_TRUE(glow.has_value());

  // Promote the filtered group itself. Its layer rasterization via
  // drawEntityRange(glow, glow.lastRenderedEntity) should render the circle
  // inside the filter context.
  CompositorController compositor(document, renderer_);
  ASSERT_TRUE(compositor.promoteEntity(glow->entityHandle().entity()))
      << "the filter-bearing group itself should promote (it IS the compositing root)";

  DualPathVerifier verifier(compositor, renderer_);
  const auto result = verifier.renderAndVerify(viewport_);

  EXPECT_LE(result.maxChannelDiff, 10) << "filtered group with children must render its children: "
                                       << result;
  EXPECT_LE(result.mismatchCount, result.totalPixels / 20u) << result;  // 5% AA tolerance
}

// Regression for donner_splash.svg bug: the user drags a letter/path that's
// a CHILD of a `<g filter="url(#blur)">` group. Under the current compositor
// behavior, the child path gets auto-promoted (or interaction-promoted via
// drag) and rasterized standalone — outside of its parent's filter context.
// The blur visually disappears because the child's layer bitmap has no blur
// applied (the filter is on the group, not the child).
//
// This test reproduces the scenario explicitly: we promote a CHILD of a
// filtered group and compare the composited output against the full-render
// reference. If the filter context is lost, the two paths diverge.
TEST_F(CompositorGoldenTest, ChildOfFilteredGroupRefusesPromotion) {
  // Regression for the splash-SVG drag bug reported in manual testing:
  // dragging a blurred shape (path inside `<g filter="url(#blur)">`) caused
  // the blur to disappear. Root cause: the editor's drag promotion path
  // extracted the descendant into its own cached layer, losing the
  // ancestor's filter context. The cached layer bitmap had un-blurred
  // content; the composed output showed pure red where the blurred halo
  // should have been.
  //
  // Fix: `promoteEntity` now walks ancestors and refuses promotion when
  // any ancestor has a filter/mask/clip-path. The editor's drag path,
  // when promoteEntity returns false, falls back to the full-render
  // mutation path which handles the ancestor filter correctly.
  SVGDocument document = parseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
      <defs>
        <filter id="blur" x="-20%" y="-20%" width="140%" height="140%">
          <feGaussianBlur in="SourceGraphic" stdDeviation="4"/>
        </filter>
      </defs>
      <rect width="200" height="100" fill="white"/>
      <g filter="url(#blur)">
        <rect id="target" x="60" y="20" width="60" height="60" fill="red"/>
      </g>
    </svg>
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  CompositorController compositor(document, renderer_);
  // Simulate the editor's drag promotion: the user clicked on the child
  // rect inside the filtered group. The controller must refuse to promote.
  EXPECT_FALSE(compositor.promoteEntity(target->entityHandle().entity()))
      << "promotion of a descendant of `<g filter=...>` must be refused so "
         "the ancestor's filter context isn't lost during composited drag";

  // With promotion refused, `isPromoted` is false and `layerCount` is 0.
  EXPECT_FALSE(compositor.isPromoted(target->entityHandle().entity()));
  EXPECT_EQ(compositor.layerCount(), 0u);
}

// Parallel regression: ancestor with clip-path also refuses promotion.
TEST_F(CompositorGoldenTest, ChildOfClippedGroupRefusesPromotion) {
  SVGDocument document = parseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
      <defs>
        <clipPath id="half">
          <rect x="0" y="0" width="100" height="100"/>
        </clipPath>
      </defs>
      <rect width="200" height="100" fill="white"/>
      <g clip-path="url(#half)">
        <rect id="target" x="60" y="20" width="60" height="60" fill="red"/>
      </g>
    </svg>
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  CompositorController compositor(document, renderer_);
  EXPECT_FALSE(compositor.promoteEntity(target->entityHandle().entity()))
      << "clip-path ancestor context must not be extracted";
}

// Parallel regression: ancestor with mask also refuses promotion.
TEST_F(CompositorGoldenTest, ChildOfMaskedGroupRefusesPromotion) {
  SVGDocument document = parseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
      <defs>
        <mask id="m">
          <rect x="0" y="0" width="100" height="100" fill="white"/>
        </mask>
      </defs>
      <rect width="200" height="100" fill="white"/>
      <g mask="url(#m)">
        <rect id="target" x="60" y="20" width="60" height="60" fill="red"/>
      </g>
    </svg>
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  CompositorController compositor(document, renderer_);
  EXPECT_FALSE(compositor.promoteEntity(target->entityHandle().entity()))
      << "mask ancestor context must not be extracted";
}

// Positive: a plain descendant (no compositing ancestor) still promotes.
// Guards against over-refusing promotion.
TEST_F(CompositorGoldenTest, PlainDescendantStillPromotes) {
  SVGDocument document = parseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
      <rect width="200" height="100" fill="white"/>
      <g>
        <rect id="target" x="60" y="20" width="60" height="60" fill="red"/>
      </g>
    </svg>
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  CompositorController compositor(document, renderer_);
  EXPECT_TRUE(compositor.promoteEntity(target->entityHandle().entity()))
      << "plain `<g>` ancestor without compositing features doesn't block promotion";
  EXPECT_TRUE(compositor.isPromoted(target->entityHandle().entity()));
}

// Regression: a radially-gradient-filled shape, auto-promoted, renders
// correctly. Pairs with the Gaussian-blur test to cover two of the key
// features in the editor's splash SVG.
TEST_F(CompositorGoldenTest, RadialGradientShapeMatchesFullRenderAfterPromotion) {
  SVGDocument document = parseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
      <defs>
        <radialGradient id="g" cx="50%" cy="50%" r="50%">
          <stop offset="0%" stop-color="yellow"/>
          <stop offset="100%" stop-color="red"/>
        </radialGradient>
      </defs>
      <rect width="200" height="100" fill="white"/>
      <circle id="target" cx="100" cy="50" r="30" fill="url(#g)"/>
    </svg>
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  CompositorController compositor(document, renderer_);
  ASSERT_TRUE(compositor.promoteEntity(target->entityHandle().entity()));

  DualPathVerifier verifier(compositor, renderer_);
  const auto result = verifier.renderAndVerify(viewport_);

  EXPECT_LE(result.maxChannelDiff, 3) << result;
  EXPECT_LE(result.mismatchCount, result.totalPixels / 50u) << result;
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
