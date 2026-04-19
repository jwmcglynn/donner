#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>

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
  target->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(Vector2d(100.0, 0.0)));
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
  target->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(Vector2d(100.0, 0.0)));
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
  compositor.renderFrame(viewport_);

  // Phase 2: resetAllLayers, then promote + render again.
  compositor.resetAllLayers();
  ASSERT_TRUE(compositor.promoteEntity(target->entityHandle().entity()));
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
  compositor.renderFrame(viewport_);

  // Phase 2 — drag via DOM mutation. The compositor's fast path detects the
  // pure-translation delta vs. the bitmap's stamped transform and reuses the
  // cached bitmap with an internal compose offset.
  target->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(Vector2d(100.0, 0.0)));
  compositor.renderFrame(viewport_);

  // Phase 3 — DOM is already at the final transform; just re-render.
  compositor.renderFrame(viewport_);

  // Phase 4 — resetAllLayers, re-promote.
  compositor.resetAllLayers();
  ASSERT_TRUE(compositor.promoteEntity(entity));
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

  // DOM is at identity — the compositor and reference paths render the same
  // pixels. Each renderFrame internally dual-paths and asserts. Survival to
  // the end = no drift detected.
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
  compositor.renderFrame(viewport_);

  // Now simulate a drag: mutate the DOM transform. The cached bitmap should
  // still carry the blur; the compositor's fast path just repositions it
  // via an internal compose offset.
  target->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(Vector2d(30.0, 0.0)));
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

  // Series of drag frames (editor drags at ~60fps). The user moves the DOM
  // directly; the compositor's fast path picks up the pure-translation delta
  // and reuses the cached bitmap via an internal compose offset.
  Pixel glowDuringDrag{};
  for (int i = 1; i <= 10; ++i) {
    letterA->cast<SVGGraphicsElement>().setTransform(
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
    letter2->cast<SVGGraphicsElement>().setTransform(
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
  target->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(Vector2d(1.0, 0.0)));
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
    target->cast<SVGGraphicsElement>().setTransform(
        Transform2d::Translate(Vector2d(i * 1.0, 0.0)));
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

// A drag-release `SetTransformCommand` mutation must NOT trigger a full
// `prepareDocumentForRendering` rebuild — that tears down every RIC and
// every `ComputedShadowTreeComponent`, then recomputes styles / paint /
// filters across the whole tree (O(N)). For a complex document this was
// the user-visible ~2s hang on mouse release even though nothing needed
// to change outside the single dragged entity. The compositor's fast-path
// catches "only layout/transform dirty flags on promoted-layer-root
// entities" and refreshes just the affected `entityFromWorldTransform`
// in place, leaving the rest of the render tree untouched.
//
// Test probes a RIC pointer outside the dirty entity's layer — it must
// stay stable across the mutation, which is only true if no RIC rebuild
// happened.
TEST_F(CompositorGoldenTest, TransformMutationOnPromotedEntitySkipsFullPrepare) {
  SVGDocument document = parseDocument(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
  <rect id="bystander" x="100" y="10" width="50" height="50" fill="blue"/>
  <rect id="target" x="10" y="10" width="50" height="50" fill="red"/>
</svg>
  )svg");

  auto bystander = document.querySelector("#bystander");
  auto target = document.querySelector("#target");
  ASSERT_TRUE(bystander.has_value());
  ASSERT_TRUE(target.has_value());
  const Entity bystanderEntity = bystander->entityHandle().entity();

  CompositorController compositor(document, renderer_);
  ASSERT_TRUE(compositor.promoteEntity(target->entityHandle().entity()));

  compositor.renderFrame(viewport_);

  // Capture the bystander's RIC address. `prepareDocumentForRendering`
  // clears and re-emplaces `RenderingInstanceComponent`, so the object's
  // storage address changes across the rebuild. If the fast path works,
  // the bystander's RIC stays put.
  const components::RenderingInstanceComponent* bystanderRicBefore =
      &document.registry().get<components::RenderingInstanceComponent>(bystanderEntity);

  // Simulate the drag-release mutation on the promoted target.
  target->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(20.0, 0.0));
  compositor.renderFrame(viewport_);

  const components::RenderingInstanceComponent* bystanderRicAfter =
      &document.registry().get<components::RenderingInstanceComponent>(bystanderEntity);
  EXPECT_EQ(bystanderRicBefore, bystanderRicAfter)
      << "unrelated entity's RIC was reallocated — prepareDocumentForRendering ran and wiped every "
         "RIC. The compositor should have taken the fast path and updated only the target's RIC.";
}

// A SetTransformCommand on drag release dirties the dragged entity. The
// compositor must re-rasterize *only* the drag layer's cached bitmap — NOT
// every mandatory filter layer it happens to share the document with. A
// naive `any-entity-dirty → root-dirty → rasterize-everything` policy would
// pay full filter-rasterization cost on every drag release. The test sets
// up a drag layer alongside a filter layer, simulates the drag-release
// mutation, and asserts the filter layer's cached bitmap is preserved
// (same `.data()` pointer).
TEST_F(CompositorGoldenTest, DragEntityMutationKeepsMandatoryFilterLayerCached) {
  SVGDocument document = parseDocument(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
  <defs><filter id="blur"><feGaussianBlur in="SourceGraphic" stdDeviation="3"/></filter></defs>
  <rect width="200" height="100" fill="white"/>
  <g id="glow" filter="url(#blur)"><circle cx="150" cy="50" r="20" fill="yellow"/></g>
  <rect id="target" x="10" y="10" width="50" height="50" fill="red"/>
</svg>
  )svg");
  auto glow = document.querySelector("#glow");
  auto target = document.querySelector("#target");
  ASSERT_TRUE(glow.has_value());
  ASSERT_TRUE(target.has_value());

  CompositorController compositor(document, renderer_);
  ASSERT_TRUE(compositor.promoteEntity(target->entityHandle().entity()));

  // Initial render populates every layer bitmap and bg/fg.
  compositor.renderFrame(viewport_);

  // Capture the pre-mutation filter-layer bitmap pointer. An intervening
  // rasterizeLayer call would reallocate the vector and change `.data()`.
  const uint8_t* glowDataBefore = compositor.layerBitmapOf(glow->entityHandle().entity()).pixels.data();
  ASSERT_NE(glowDataBefore, nullptr);

  // Simulate the mutation the editor applies on drag release: bake the drag
  // translation into the element's transform attribute.
  target->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(30.0, 0.0));

  // Settling render.
  compositor.renderFrame(viewport_);

  const uint8_t* glowDataAfter = compositor.layerBitmapOf(glow->entityHandle().entity()).pixels.data();
  EXPECT_EQ(glowDataBefore, glowDataAfter)
      << "glow filter layer must be reused across the drag-release mutation — re-rasterizing it "
         "is what the user felt as a ~2s hang on every new drag";
}

// Phase B per-segment isolation: mutating a non-promoted entity in one
// paint-order slot must NOT invalidate segments in other slots. Document
// has two non-promoted rects (`before`, `after`) on either side of a
// promoted drag target. The `before` rect lives in segment[0]; `after`
// lives in segment[1]. We capture both segments' data pointers, mutate
// only `after`, and assert segment[0]'s pointer is preserved — i.e.
// segment[0] stayed cached because only segment[1] was dirty.
// Mirrors the splash shape: one drag target plus N sibling mandatory-promoted
// filter groups, then drives many drag frames. Each drag frame only mutates
// the drag target's transform — every filter layer's bitmap MUST be reused
// across the whole drag session. Before the fix, mutating the drag target
// somehow escalated to marking sibling filter layers dirty (or to
// `rootDirty_`), so `rasterizeLayer()` got called per-filter per drag frame.
// Each rasterize calls `renderer_->createOffscreenInstance()`; after enough
// frames the extra offscreen traffic becomes the hot path the user felt as
// "~200ms per drag update" and is also what was tripping the crash in
// `Renderer::createOffscreenInstance() + 36` (a thrashed renderer state, see
// the SIGSEGV signature in the regression report).
//
// Probe: capture every filter layer's bitmap `.data()` pointer after the first
// drag frame, then drive N more drag frames. The pointer stays stable iff the
// vector was never reassigned — i.e. the layer was not re-rasterized. Any
// regression that makes a filter layer re-rasterize during pure-translation
// drag will flip the pointer.
TEST_F(CompositorGoldenTest, SplashDragMultipleFilterLayersStableAcrossManyFrames) {
  SVGDocument document = parseDocument(R"svg(
<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" width="892" height="512" viewBox="0 0 892 512">
  <defs>
    <filter id="blur-a"><feGaussianBlur in="SourceGraphic" stdDeviation="4.5"/></filter>
    <filter id="blur-b"><feGaussianBlur in="SourceGraphic" stdDeviation="6"/></filter>
    <filter id="blur-c"><feGaussianBlur in="SourceGraphic" stdDeviation="8"/></filter>
  </defs>
  <rect width="892" height="512" fill="#0d0f1d"/>
  <g id="glow_a" filter="url(#blur-a)">
    <rect x="170" y="390" width="80" height="80" fill="#ffe54a"/>
  </g>
  <g id="Donner">
    <rect id="letter_1" x="270" y="345" width="70" height="90" fill="#fae100"/>
    <rect id="letter_2" x="350" y="345" width="70" height="90" fill="#fae100"/>
    <rect id="letter_3" x="430" y="345" width="70" height="90" fill="#fae100"/>
  </g>
  <g id="glow_b" filter="url(#blur-b)">
    <rect x="435" y="380" width="80" height="80" fill="#ffe54a"/>
  </g>
  <g id="glow_c" filter="url(#blur-c)">
    <rect x="670" y="370" width="80" height="80" fill="#ffe54a"/>
  </g>
</svg>
  )svg");

  RenderViewport splashViewport;
  splashViewport.size = Vector2d(892, 512);
  splashViewport.devicePixelRatio = 1.0;

  auto letter2 = document.querySelector("#letter_2");
  auto glowA = document.querySelector("#glow_a");
  auto glowB = document.querySelector("#glow_b");
  auto glowC = document.querySelector("#glow_c");
  ASSERT_TRUE(letter2.has_value());
  ASSERT_TRUE(glowA.has_value());
  ASSERT_TRUE(glowB.has_value());
  ASSERT_TRUE(glowC.has_value());

  const Entity letter2Entity = letter2->entityHandle().entity();
  const Entity glowAEntity = glowA->entityHandle().entity();
  const Entity glowBEntity = glowB->entityHandle().entity();
  const Entity glowCEntity = glowC->entityHandle().entity();

  // Editor default config: all auto-promotion sources on.
  CompositorConfig config;
  CompositorController compositor(document, renderer_, config);

  // Pre-warm: simulate the selection-pre-warm phase the real editor goes
  // through before the user's first mouse-move. The mandatory-filter
  // detector runs, finds the three `<g filter=...>` groups, publishes
  // `Mandatory` hints on each; the drag target gets an `Interaction` hint.
  // All four layers rasterize. `bg` / `fg` get composited.
  ASSERT_TRUE(compositor.promoteEntity(letter2Entity));
  compositor.renderFrame(splashViewport);

  // First drag frame. Stamp the drag layer's bitmap at offset (4, 0). This
  // is the baseline for the "bitmap must stay reused" claim below.
  letter2->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(Vector2d(4.0, 0.0)));
  compositor.renderFrame(splashViewport);

  const uint8_t* glowAData = compositor.layerBitmapOf(glowAEntity).pixels.data();
  const uint8_t* glowBData = compositor.layerBitmapOf(glowBEntity).pixels.data();
  const uint8_t* glowCData = compositor.layerBitmapOf(glowCEntity).pixels.data();
  ASSERT_NE(glowAData, nullptr);
  ASSERT_NE(glowBData, nullptr);
  ASSERT_NE(glowCData, nullptr);

  // Drive many more drag frames, pure-translation only — no filter layer's
  // pixel content has changed, so no filter layer should re-rasterize.
  constexpr int kTrailingDragFrames = 20;
  for (int i = 2; i <= kTrailingDragFrames; ++i) {
    letter2->cast<SVGGraphicsElement>().setTransform(
        Transform2d::Translate(Vector2d(static_cast<double>(i) * 4.0, 0.0)));
    compositor.renderFrame(splashViewport);
  }

  // Each filter layer's bitmap pointer must still match — the vector was not
  // reallocated, i.e. no `rasterizeLayer` ran for these layers. If this trips,
  // the compositor has regressed into a mode where drag-target mutations
  // cascade into the filter layers, which means the splash is re-rasterizing
  // every frame (the perf symptom) and repeatedly hitting
  // `Renderer::createOffscreenInstance()` (the crash symptom).
  EXPECT_EQ(compositor.layerBitmapOf(glowAEntity).pixels.data(), glowAData)
      << "glow_a filter-layer bitmap re-allocated during drag — the drag-target mutation "
         "escalated to marking the filter layer dirty.";
  EXPECT_EQ(compositor.layerBitmapOf(glowBEntity).pixels.data(), glowBData)
      << "glow_b filter-layer bitmap re-allocated during drag.";
  EXPECT_EQ(compositor.layerBitmapOf(glowCEntity).pixels.data(), glowCData)
      << "glow_c filter-layer bitmap re-allocated during drag.";
}

// Drag target is a `<g>` with child elements, so its entity range is multi-
// entity (`subtreeInfo.lastRenderedEntity != firstEntity`). The fast-path
// single-entity eligibility check rejects this layer; every drag frame falls
// through to the slow path — `prepareDocumentForRendering` + a full
// `rasterizeLayer` call. That slow path is where the user's ~200ms drag
// updates and the `Renderer::createOffscreenInstance` crash live.
//
// This test drives many drag frames against a multi-entity drag layer in a
// splash-shaped document (sibling filter groups on both sides of the drag
// target) and asserts that every frame completes without crashing. Before
// the fix the `rasterizeLayer` call path was flaky against repeated offscreen
// instantiation — exactly the `+36` bytes-into-`createOffscreenInstance`
// SIGSEGV the editor hit.
TEST_F(CompositorGoldenTest, SplashDragOnGroupReExercisesRasterizePath) {
  SVGDocument document = parseDocument(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="892" height="512" viewBox="0 0 892 512">
  <defs>
    <filter id="blur-a"><feGaussianBlur in="SourceGraphic" stdDeviation="4.5"/></filter>
    <filter id="blur-b"><feGaussianBlur in="SourceGraphic" stdDeviation="6"/></filter>
  </defs>
  <rect width="892" height="512" fill="#0d0f1d"/>
  <g id="glow_a" filter="url(#blur-a)">
    <rect x="170" y="390" width="80" height="80" fill="#ffe54a"/>
  </g>
  <g id="Donner_target">
    <path d="M270,345 L340,345 L340,435 L270,435 Z" fill="#fae100"/>
    <path d="M350,345 L420,345 L420,435 L350,435 Z" fill="#fae100"/>
  </g>
  <g id="glow_b" filter="url(#blur-b)">
    <rect x="435" y="380" width="80" height="80" fill="#ffe54a"/>
  </g>
</svg>
  )svg");

  RenderViewport splashViewport;
  splashViewport.size = Vector2d(892, 512);
  splashViewport.devicePixelRatio = 1.0;

  auto donner = document.querySelector("#Donner_target");
  ASSERT_TRUE(donner.has_value());
  const Entity donnerEntity = donner->entityHandle().entity();

  CompositorConfig config;
  CompositorController compositor(document, renderer_, config);

  // First render to populate RICs and detect mandatory filter hints.
  compositor.renderFrame(splashViewport);
  // Now promote the `<g>` — a multi-entity range.
  ASSERT_TRUE(compositor.promoteEntity(donnerEntity));

  // Drive many drag frames. If any frame crashes inside `rasterizeLayer`
  // because of a bad renderer state, the test fails via SIGSEGV. If it
  // completes without crashing, we've retained the correctness invariant.
  constexpr int kDragFrames = 30;
  for (int i = 1; i <= kDragFrames; ++i) {
    donner->cast<SVGGraphicsElement>().setTransform(
        Transform2d::Translate(Vector2d(static_cast<double>(i) * 2.0, 0.0)));
    compositor.renderFrame(splashViewport);
  }

  // Sanity: the drag target is still promoted after the sequence.
  EXPECT_TRUE(compositor.isPromoted(donnerEntity));
}

TEST_F(CompositorGoldenTest, NonPromotedMutationInvalidatesOnlyContainingSegment) {
  SVGDocument document = parseDocument(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
  <rect id="before" x="0" y="0" width="40" height="40" fill="green"/>
  <rect id="target" x="60" y="0" width="40" height="40" fill="red"/>
  <rect id="after" x="120" y="0" width="40" height="40" fill="blue"/>
</svg>
  )svg");
  auto before = document.querySelector("#before");
  auto target = document.querySelector("#target");
  auto after = document.querySelector("#after");
  ASSERT_TRUE(before.has_value());
  ASSERT_TRUE(target.has_value());
  ASSERT_TRUE(after.has_value());

  CompositorController compositor(document, renderer_);
  ASSERT_TRUE(compositor.promoteEntity(target->entityHandle().entity()));

  // First render populates both segments.
  compositor.renderFrame(viewport_);

  // `backgroundBitmap()` is segment[0] via the N=1 fast path; this is the
  // public handle we can observe. Capture its data pointer — the vector
  // gets reassigned on re-rasterize, so the pointer is a direct probe of
  // whether segment[0] was re-rasterized.
  const RendererBitmap& bgBefore = compositor.backgroundBitmap();
  ASSERT_FALSE(bgBefore.empty());
  const uint8_t* bgDataBefore = bgBefore.pixels.data();

  // Mutate `after` (lives in segment[1]) — per-segment dirty tracking
  // should flag only segment[1].
  after->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(5.0, 0.0));
  compositor.renderFrame(viewport_);

  const RendererBitmap& bgAfter = compositor.backgroundBitmap();
  ASSERT_FALSE(bgAfter.empty());
  EXPECT_EQ(bgDataBefore, bgAfter.pixels.data())
      << "segment[0] re-rasterized even though the mutation lived in segment[1] — "
         "Phase B per-segment dirty tracking regressed. `rootDirty_` escalation "
         "has crept back in somewhere in consumeDirtyFlags.";
}

// Elements whose transformed device-space bounds fall entirely outside the
// render target (after including stroke width) must be skipped by the
// renderer's core culling path — drawing them wastes GPU/CPU cycles and
// their contribution is zero. The test puts a red 10×10 rect well above the
// visible canvas and asserts the final image is entirely white.
TEST_F(CompositorGoldenTest, ViewportCullingSkipsOffscreenPaths) {
  SVGDocument document = parseDocument(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
  <rect width="100" height="100" fill="white"/>
  <rect x="10" y="-500" width="10" height="10" fill="red"/>
</svg>
  )svg");

  document.setCanvasSize(100, 100);
  RenderViewport vp;
  vp.size = Vector2d(100, 100);
  vp.devicePixelRatio = 1.0;
  CompositorController compositor(document, renderer_);
  compositor.renderFrame(vp);
  const RendererBitmap bitmap = renderer_.takeSnapshot();

  EXPECT_THAT(getPixel(bitmap, 50, 50), IsWhite());
  EXPECT_THAT(getPixel(bitmap, 15, 10), IsWhite())
      << "off-screen red rect would only show here if the renderer had a bug that displaced it; "
         "mainly the test proves we don't crash on culled content";
}

// Paths whose entire device-space bounds are smaller than a quarter pixel
// contribute nothing perceptible even with AA, so the renderer culls them.
// A 0.01×0.01 red rect at the center must not discolor the white background.
TEST_F(CompositorGoldenTest, TooSmallCullingSkipsSubpixelPaths) {
  SVGDocument document = parseDocument(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
  <rect width="100" height="100" fill="white"/>
  <rect x="50" y="50" width="0.01" height="0.01" fill="red"/>
</svg>
  )svg");

  document.setCanvasSize(100, 100);
  RenderViewport vp;
  vp.size = Vector2d(100, 100);
  vp.devicePixelRatio = 1.0;
  CompositorController compositor(document, renderer_);
  compositor.renderFrame(vp);
  const RendererBitmap bitmap = renderer_.takeSnapshot();

  EXPECT_THAT(getPixel(bitmap, 50, 50), IsWhite());
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
  <rect x="130" y="10" width="50" height="50" fill="blue"/>
</svg>
  )svg");
  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  CompositorController compositor(document, renderer_);

  ASSERT_TRUE(compositor.promoteEntity(target->entityHandle().entity()));
  target->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(Vector2d(1.0, 0.0)));

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
    letter->cast<SVGGraphicsElement>().setTransform(
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
    letterD->cast<SVGGraphicsElement>().setTransform(
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

  compositor.renderFrame(viewport_);

  letterD->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(Vector2d(20.0, 10.0)));
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
  compositor.renderFrame(viewport_);

  // Frame 2: user starts dragging letter D.
  letterD->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(Vector2d(20.0, 10.0)));
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

// Splash-shape document, real Skia-backed renderer, full-size 892×512
// canvas — the exact conditions the editor hits when the user opens
// `donner_splash.svg` and drags a letter. Measures each latency bucket
// the user feels:
//
//   1. Cold first render (no compositor state yet)
//   2. First drag frame (first promote, first segment + layer rasterization)
//   3. Steady-state drag frame (fast path, should be near-zero)
//   4. Post-drag reset + re-render (simulates the source-writeback reparse
//      that fires after drag release)
//
// Thresholds are intentionally loose — they're "something regressed by 2x"
// gates, not tight perf targets. The interactive-feel number is #3: if
// steady-state drag blows past a handful of ms, dragging feels laggy.
TEST_F(CompositorGoldenTest, SplashDragLatencyBudgetsOnRealRenderer) {
  const char* kSplashSource = R"svg(
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
  )svg";

  SVGDocument document = parseDocument(kSplashSource);

  RenderViewport fullViewport;
  fullViewport.size = Vector2d(892, 512);
  fullViewport.devicePixelRatio = 1.0;

  // Editor default config: all auto-promotion features on. `setSkipMainCompose
  // DuringSplit(true)` matches the editor's setting — without it the compositor
  // pays ~100 ms/frame on the real renderer composing into a buffer the editor
  // never displays during drag.
  CompositorConfig config;
  CompositorController compositor(document, renderer_, config);
  compositor.setSkipMainComposeDuringSplit(true);

  using Clock = std::chrono::steady_clock;
  const auto elapsedMs = [](Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
  };

  // Phase 1 — cold first render (no drag promoted yet).
  const auto t0 = Clock::now();
  compositor.renderFrame(fullViewport);
  const double coldRenderMs = elapsedMs(t0);

  auto letter2 = document.querySelector("#letter_2");
  ASSERT_TRUE(letter2.has_value());
  const Entity letter2Entity = letter2->entityHandle().entity();

  // Phase 2 — first drag frame. Promotes the drag target for the first
  // time, triggers first-time rasterization of N+1 segments + all promoted
  // layer bitmaps + bg/fg composite.
  ASSERT_TRUE(compositor.promoteEntity(letter2Entity));
  letter2->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(Vector2d(4.0, 0.0)));
  const auto t1 = Clock::now();
  compositor.renderFrame(fullViewport);
  const double firstDragFrameMs = elapsedMs(t1);

  // Phase 3 — steady-state drag frames. Fast path updates composition
  // transform, skip-main-compose skips the main-renderer draw. Should be
  // sub-millisecond on any hardware; allow headroom for slow CI.
  double steadyMaxMs = 0.0;
  double steadyTotalMs = 0.0;
  constexpr int kSteadyFrames = 20;
  for (int i = 2; i <= kSteadyFrames + 1; ++i) {
    letter2->cast<SVGGraphicsElement>().setTransform(
        Transform2d::Translate(Vector2d(static_cast<double>(i) * 4.0, 0.0)));
    const auto tFrame = Clock::now();
    compositor.renderFrame(fullViewport);
    const double frameMs = elapsedMs(tFrame);
    steadyMaxMs = std::max(steadyMaxMs, frameMs);
    steadyTotalMs += frameMs;
  }
  const double steadyAvgMs = steadyTotalMs / kSteadyFrames;

  // Phase 4 — drag-end reset. Simulates what happens in the editor when
  // the source-writeback round-trip issues a `ReplaceDocumentCommand`:
  // `resetAllLayers` clears every cache, then the next render rebuilds
  // from scratch. The user feels this as a freeze right after letting go
  // of the drag.
  compositor.resetAllLayers();
  const auto t4 = Clock::now();
  compositor.renderFrame(fullViewport);
  const double postResetRenderMs = elapsedMs(t4);

  std::cerr << "[PERF] SplashDragLatency: cold=" << coldRenderMs
            << " ms, firstDrag=" << firstDragFrameMs << " ms, steadyAvg=" << steadyAvgMs
            << " ms, steadyMax=" << steadyMaxMs << " ms, postReset=" << postResetRenderMs
            << " ms\n";

  // Budgets are loose — real Skia on an M1 lands cold ≈ 600-800 ms,
  // first-drag ≈ 3000-3500 ms (regression target), steady ≈ 0.2 ms,
  // post-reset ≈ 3000-4000 ms (regression target).
  //
  // The steady budget is the one that directly determines "does drag
  // feel smooth". Anything above ~20 ms will show as perceptible lag
  // during a 60 Hz mouse-move stream.
  EXPECT_LT(steadyAvgMs, 20.0) << "steady-state drag average is above 20 ms — this is the "
                                  "interactive-feel budget; >20 ms shows as perceptible lag";
  EXPECT_LT(steadyMaxMs, 50.0) << "a single steady-state drag frame spiked above 50 ms";

  // First-drag-frame and post-reset budgets are deliberately permissive
  // in v1 — these failures are known-expensive. Tighten them as fixes
  // land for Tasks #26 / #27.
  EXPECT_LT(firstDragFrameMs, 5000.0)
      << "first drag frame exploded past 5s — something regressed the first-time promote path";
  EXPECT_LT(postResetRenderMs, 5000.0)
      << "post-reset rebuild exploded past 5s — something regressed resetAllLayers+rebuild";
}

// Same splash document + same drag mutation, but promotes the target BEFORE
// the first render — simulating the ideal editor flow where selection
// fires a pre-warm render before the user starts dragging. The expensive
// first-time rasterization moves to the pre-warm; the first drag frame
// then hits the steady-state fast path. This test proves that the big
// first-drag spike is an ordering problem (pre-warm happens too late),
// not an intrinsic cost.
TEST_F(CompositorGoldenTest, SplashPrewarmMakesFirstDragFree) {
  const char* kSplashSource = R"svg(
<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" width="892" height="512" viewBox="0 0 892 512">
  <defs>
    <filter id="blur-a"><feGaussianBlur in="SourceGraphic" stdDeviation="4.5"/></filter>
    <filter id="blur-b"><feGaussianBlur in="SourceGraphic" stdDeviation="6"/></filter>
    <filter id="blur-c"><feGaussianBlur in="SourceGraphic" stdDeviation="8"/></filter>
  </defs>
  <g class="wrapper">
    <rect width="892" height="512" fill="#0d0f1d"/>
    <g id="glow_behind" filter="url(#blur-a)">
      <rect x="170" y="390" width="80" height="80" fill="yellow" fill-opacity="0.5"/>
    </g>
    <rect id="letter_2" x="350" y="345" width="70" height="90" fill="red"/>
    <g id="glow_middle" filter="url(#blur-b)">
      <rect x="435" y="380" width="80" height="80" fill="yellow" fill-opacity="0.5"/>
    </g>
    <g id="glow_foreground" filter="url(#blur-c)">
      <rect x="670" y="370" width="80" height="80" fill="yellow" fill-opacity="0.5"/>
    </g>
  </g>
</svg>
  )svg";

  SVGDocument document = parseDocument(kSplashSource);

  RenderViewport fullViewport;
  fullViewport.size = Vector2d(892, 512);
  fullViewport.devicePixelRatio = 1.0;

  CompositorConfig config;
  CompositorController compositor(document, renderer_, config);
  compositor.setSkipMainComposeDuringSplit(true);

  auto letter2 = document.querySelector("#letter_2");
  ASSERT_TRUE(letter2.has_value());
  const Entity letter2Entity = letter2->entityHandle().entity();

  using Clock = std::chrono::steady_clock;
  const auto elapsedMs = [](Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
  };

  // Promote BEFORE the first render — pre-warm flow.
  ASSERT_TRUE(compositor.promoteEntity(letter2Entity));

  const auto t0 = Clock::now();
  compositor.renderFrame(fullViewport);  // single cold+prewarm render
  const double coldRenderMs = elapsedMs(t0);

  // First "drag frame" — target already promoted, caches already warm.
  // Should hit the fast path and skip main compose.
  letter2->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(Vector2d(4.0, 0.0)));
  const auto t1 = Clock::now();
  compositor.renderFrame(fullViewport);
  const double firstDragFrameMs = elapsedMs(t1);

  std::cerr << "[PERF] SplashPrewarm: cold+prewarm=" << coldRenderMs
            << " ms, firstDrag=" << firstDragFrameMs << " ms\n";

  // With promote-before-first-render, the expensive rasterization lands on
  // the cold pre-warm render, and the first drag frame is free.
  EXPECT_LT(firstDragFrameMs, 20.0)
      << "promote-before-first-render should give a steady-state first drag frame; "
         "cost is appearing on the drag frame, which means the pre-warm didn't actually warm";
}

// Run the EXACT `donner_splash.svg` (112 paths, filter groups with real
// geometry, CSS clip-paths) through the drag-latency harness so the
// numbers this file emits match what the user sees in the editor. My
// reduced splash has ~10 drawable rects and reports ~400 ms for the
// first drag frame on TinySkia; the real splash reports ~3200 ms in the
// editor — the 8× gap is document complexity, not backend choice (both
// paths are `RendererTinySkia` by default per `config/extensions.bzl`).
//
// The test file is in the `data` array of the `compositor_golden_tests`
// target; bazel materializes it under the test's runfiles directory.
TEST_F(CompositorGoldenTest, RealSplashDragLatencyOnTinySkia) {
  // Load the splash file from the Bazel runfiles. If the path can't be
  // opened the test skips — a dev running the test outside bazel (e.g.
  // via an IDE runner that bypasses runfiles plumbing) shouldn't see a
  // spurious failure.
  const char* kSplashPath = "donner_splash.svg";
  std::ifstream splashStream(kSplashPath);
  if (!splashStream.is_open()) {
    GTEST_SKIP() << "donner_splash.svg not found in runfiles — skipping splash perf test";
  }
  std::ostringstream splashBuf;
  splashBuf << splashStream.rdbuf();
  const std::string splashSource = splashBuf.str();
  ASSERT_FALSE(splashSource.empty()) << "donner_splash.svg is empty";

  SVGDocument document = parseDocument(splashSource);

  RenderViewport fullViewport;
  fullViewport.size = Vector2d(892, 512);
  fullViewport.devicePixelRatio = 1.0;

  CompositorConfig config;
  CompositorController compositor(document, renderer_, config);
  compositor.setSkipMainComposeDuringSplit(true);

  using Clock = std::chrono::steady_clock;
  const auto elapsedMs = [](Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
  };

  // Phase 1 — cold first render (no drag promoted yet).
  const auto t0 = Clock::now();
  compositor.renderFrame(fullViewport);
  const double coldRenderMs = elapsedMs(t0);

  // Find a draggable leaf. The splash's `Donner` group has path children
  // (the letters); each is a simple `<path>` with no children, which is
  // exactly the shape the fast path wants. Grab the first one.
  auto firstLetter = document.querySelector("#Donner path");
  ASSERT_TRUE(firstLetter.has_value())
      << "splash has no `#Donner path` — has the file structure changed?";
  const Entity letterEntity = firstLetter->entityHandle().entity();

  // Phase 2 — first drag frame. Promotes + rasterizes everything.
  ASSERT_TRUE(compositor.promoteEntity(letterEntity))
      << "letter failed to promote — a compositing-breaking ancestor may be in the way";
  firstLetter->cast<SVGGraphicsElement>().setTransform(
      Transform2d::Translate(Vector2d(4.0, 0.0)));
  const auto t1 = Clock::now();
  compositor.renderFrame(fullViewport);
  const double firstDragFrameMs = elapsedMs(t1);

  // Phase 3 — steady-state. Fast path + skipMainCompose → near-zero.
  double steadyMaxMs = 0.0;
  double steadyTotalMs = 0.0;
  constexpr int kSteadyFrames = 20;
  for (int i = 2; i <= kSteadyFrames + 1; ++i) {
    firstLetter->cast<SVGGraphicsElement>().setTransform(
        Transform2d::Translate(Vector2d(static_cast<double>(i) * 4.0, 0.0)));
    const auto tFrame = Clock::now();
    compositor.renderFrame(fullViewport);
    const double frameMs = elapsedMs(tFrame);
    steadyMaxMs = std::max(steadyMaxMs, frameMs);
    steadyTotalMs += frameMs;
  }
  const double steadyAvgMs = steadyTotalMs / kSteadyFrames;

  // Phase 4 — drag-end replay. Simulates the editor's `setDocument` →
  // `resetAllLayers(documentReplaced=true)` → fresh render sequence.
  compositor.resetAllLayers(/*documentReplaced=*/true);
  ASSERT_TRUE(compositor.promoteEntity(letterEntity));
  const auto t4 = Clock::now();
  compositor.renderFrame(fullViewport);
  const double postResetRenderMs = elapsedMs(t4);

  std::cerr << "[PERF] RealSplashDragLatency (TinySkia, real donner_splash.svg): cold="
            << coldRenderMs << " ms, firstDrag=" << firstDragFrameMs
            << " ms, steadyAvg=" << steadyAvgMs << " ms, steadyMax=" << steadyMaxMs
            << " ms, postReset=" << postResetRenderMs << " ms\n";

  // Budgets are loose — the real splash is expensive on any backend.
  // The steady-state budget is the one that matters for interactive
  // feel; anything over 20 ms shows as perceptible drag lag.
  EXPECT_LT(steadyAvgMs, 20.0) << "interactive-feel budget blown — drag will feel laggy";
  EXPECT_LT(steadyMaxMs, 100.0) << "a steady-state drag frame spiked way above budget";
  // First-drag and post-reset are known-expensive on the splash. Budget
  // at 2× the observed TinySkia numbers so an unrelated regression is
  // loud, but small fluctuations in path tessellation don't flake the
  // test. Design doc 0026 tracks the reductions in Options A/B.
  EXPECT_LT(firstDragFrameMs, 8000.0)
      << "first drag frame on real splash regressed beyond 8s — something broke beyond the "
         "known pre-existing 4s prewarm cost";
  EXPECT_LT(postResetRenderMs, 8000.0)
      << "drag-end replay on real splash regressed beyond 8s — resetAllLayers + rebuild cost "
         "grew past pre-existing baseline";
}

// The editor's drag-end path: source-pane writeback fires a
// `ReplaceDocumentCommand`, `AsyncSVGDocument::setDocument` replaces the
// inner SVGDocument in place (bumping `documentGeneration`), and
// `AsyncRenderer` responds by calling `resetAllLayers(documentReplaced=
// true)` + a fresh render. That sequence is what the user feels as a
// multi-second freeze at mouse-up on a complex document. This test
// reproduces that exact scenario against the real renderer so we can
// budget + regression-test it.
TEST_F(CompositorGoldenTest, SplashDragEndReplaceDocumentReplayLatency) {
  const char* kSplashSource = R"svg(
<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" width="892" height="512" viewBox="0 0 892 512">
  <defs>
    <filter id="blur-a"><feGaussianBlur in="SourceGraphic" stdDeviation="4.5"/></filter>
    <filter id="blur-b"><feGaussianBlur in="SourceGraphic" stdDeviation="6"/></filter>
    <filter id="blur-c"><feGaussianBlur in="SourceGraphic" stdDeviation="8"/></filter>
  </defs>
  <g class="wrapper">
    <rect width="892" height="512" fill="#0d0f1d"/>
    <g id="glow_behind" filter="url(#blur-a)">
      <rect x="170" y="390" width="80" height="80" fill="yellow" fill-opacity="0.5"/>
    </g>
    <rect id="letter_2" x="350" y="345" width="70" height="90" fill="red"/>
    <g id="glow_middle" filter="url(#blur-b)">
      <rect x="435" y="380" width="80" height="80" fill="yellow" fill-opacity="0.5"/>
    </g>
    <g id="glow_foreground" filter="url(#blur-c)">
      <rect x="670" y="370" width="80" height="80" fill="yellow" fill-opacity="0.5"/>
    </g>
  </g>
</svg>
  )svg";

  SVGDocument document = parseDocument(kSplashSource);

  RenderViewport fullViewport;
  fullViewport.size = Vector2d(892, 512);
  fullViewport.devicePixelRatio = 1.0;

  CompositorConfig config;
  CompositorController compositor(document, renderer_, config);
  compositor.setSkipMainComposeDuringSplit(true);

  auto letter2 = document.querySelector("#letter_2");
  ASSERT_TRUE(letter2.has_value());
  const Entity letter2Entity = letter2->entityHandle().entity();

  // Prewarm + drag a few frames to match the steady-state compositor state.
  ASSERT_TRUE(compositor.promoteEntity(letter2Entity));
  compositor.renderFrame(fullViewport);
  for (int i = 1; i <= 3; ++i) {
    letter2->cast<SVGGraphicsElement>().setTransform(
        Transform2d::Translate(Vector2d(static_cast<double>(i) * 4.0, 0.0)));
    compositor.renderFrame(fullViewport);
  }

  using Clock = std::chrono::steady_clock;
  const auto elapsedMs = [](Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
  };

  // Drag-end replay: compositor is told the document has been replaced.
  // `resetAllLayers(documentReplaced=true)` defuses the hints (crash-safe)
  // but loses every cached bitmap — the next render rebuilds the whole
  // thing from scratch, including re-running mandatory-hint detection,
  // re-rasterizing every promoted layer, and re-compositing bg/fg. That's
  // the "4-second hang on mouse-up" the user reports.
  compositor.resetAllLayers(/*documentReplaced=*/true);

  // Re-promote (AsyncRenderer does this because compositorEntity_ is
  // cleared after reset).
  ASSERT_TRUE(compositor.promoteEntity(letter2Entity));

  const auto t0 = Clock::now();
  compositor.renderFrame(fullViewport);
  const double replayRenderMs = elapsedMs(t0);

  std::cerr << "[PERF] SplashDragEndReplay: resetAllLayers+rerender=" << replayRenderMs << " ms\n";

  // Permissive budget — this lands ~400 ms on real-Skia in the test
  // harness, ~4000 ms on GPU-Skia in the editor. Sub-second *would* be
  // the dream but we're nowhere close without architectural changes.
  EXPECT_LT(replayRenderMs, 5000.0)
      << "drag-end replay cost exploded — `resetAllLayers(documentReplaced=true)` + rebuild is "
         "already the worst steady-state cost in the system; anything worse means something "
         "regressed the rebuild path itself";
}

// The same scenario as the drag-latency test, but asserts the CORRECTNESS
// invariant that flushed out crash #2 in the editor: after a `resetAllLayers`
// call (triggered in-editor by a drag-end `ReplaceDocumentCommand` round-
// trip through the source pane), the next `renderFrame` must not crash —
// the `ScopedCompositorHint` destructors on the cleared `activeHints_`
// map must not touch the rebuilt registry's entt sparse set. `release()` on
// each hint before clearing is what keeps this safe.
TEST_F(CompositorGoldenTest, ResetAllLayersAfterPromoteDoesNotCrash) {
  SVGDocument document = parseDocument(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
  <defs><filter id="blur"><feGaussianBlur in="SourceGraphic" stdDeviation="3"/></filter></defs>
  <rect width="200" height="100" fill="white"/>
  <g id="glow" filter="url(#blur)"><circle cx="150" cy="50" r="20" fill="yellow"/></g>
  <rect id="target" x="10" y="10" width="50" height="50" fill="red"/>
</svg>
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  CompositorController compositor(document, renderer_);
  compositor.setSkipMainComposeDuringSplit(true);

  // Promote + render — populates activeHints_ with a ScopedCompositorHint
  // pinned to the current registry. This is the state that used to crash
  // when `resetAllLayers` ran after the document was replaced.
  ASSERT_TRUE(compositor.promoteEntity(target->entityHandle().entity()));
  compositor.renderFrame(viewport_);

  // Drive a few drag frames so mandatoryDetector_ / complexityBucketer_
  // each hold their own ScopedCompositorHints too.
  for (int i = 1; i <= 3; ++i) {
    target->cast<SVGGraphicsElement>().setTransform(
        Transform2d::Translate(Vector2d(static_cast<double>(i) * 2.0, 0.0)));
    compositor.renderFrame(viewport_);
  }

  // Reset — this used to blow up inside `~ScopedCompositorHint → registry.
  // valid()` because the dtor called into entt with stale entity IDs.
  // Now the hints should be neutralized via `release()` before clear()
  // runs their dtors.
  compositor.resetAllLayers();

  // Rebuild from scratch after the reset. Must complete without crashing.
  ASSERT_TRUE(compositor.promoteEntity(target->entityHandle().entity()));
  compositor.renderFrame(viewport_);

  // A second reset-then-render cycle — the editor's source-writeback path
  // can fire this more than once if the user drags repeatedly.
  compositor.resetAllLayers();
  ASSERT_TRUE(compositor.promoteEntity(target->entityHandle().entity()));
  compositor.renderFrame(viewport_);
}

// Identity-remap baseline for `remapAfterStructuralReplace`: every old
// entity maps to itself (trivial map). This exercises the remap plumbing
// — hint re-emplacement, layer id rewrite, detector rebuild, resolver
// re-run — without needing two documents. If this regresses, the full
// cross-document remap test below is guaranteed to regress too, so
// start here when debugging.
TEST_F(CompositorGoldenTest, RemapAfterStructuralReplaceIdentityPreservesCaches) {
  SVGDocument document = parseDocument(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
  <defs><filter id="blur"><feGaussianBlur in="SourceGraphic" stdDeviation="3"/></filter></defs>
  <rect width="200" height="100" fill="white"/>
  <g id="glow" filter="url(#blur)"><circle cx="150" cy="50" r="20" fill="yellow"/></g>
  <rect id="target" x="10" y="10" width="50" height="50" fill="red"/>
</svg>
  )svg");

  auto target = document.querySelector("#target");
  auto glow = document.querySelector("#glow");
  ASSERT_TRUE(target.has_value());
  ASSERT_TRUE(glow.has_value());

  CompositorController compositor(document, renderer_);
  compositor.setSkipMainComposeDuringSplit(true);

  ASSERT_TRUE(compositor.promoteEntity(target->entityHandle().entity()));
  compositor.renderFrame(viewport_);
  // Drive a few drag frames so every cache is warm.
  for (int i = 1; i <= 3; ++i) {
    target->cast<SVGGraphicsElement>().setTransform(
        Transform2d::Translate(Vector2d(static_cast<double>(i) * 2.0, 0.0)));
    compositor.renderFrame(viewport_);
  }

  const uint8_t* glowBitmapBefore =
      compositor.layerBitmapOf(glow->entityHandle().entity()).pixels.data();
  const uint8_t* targetBitmapBefore =
      compositor.layerBitmapOf(target->entityHandle().entity()).pixels.data();

  // Build an identity remap: every entity in the registry maps to itself.
  // Walk the whole registry so we don't miss ancillary entities.
  std::unordered_map<Entity, Entity> identityRemap;
  auto& registry = document.registry();
  for (auto entity : registry.view<components::RenderingInstanceComponent>()) {
    identityRemap[entity] = entity;
  }

  ASSERT_TRUE(compositor.remapAfterStructuralReplace(identityRemap))
      << "identity remap must succeed — every required entity is present";

  // Cached bitmaps should be preserved (same `.pixels.data()` pointer).
  EXPECT_EQ(compositor.layerBitmapOf(glow->entityHandle().entity()).pixels.data(),
            glowBitmapBefore)
      << "glow filter-layer bitmap was reallocated during identity remap — cache lost";
  EXPECT_EQ(compositor.layerBitmapOf(target->entityHandle().entity()).pixels.data(),
            targetBitmapBefore)
      << "drag-target bitmap was reallocated during identity remap — cache lost";

  // Next render must stay on the steady-state fast path.
  target->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(Vector2d(10.0, 0.0)));
  compositor.renderFrame(viewport_);

  EXPECT_EQ(compositor.layerBitmapOf(glow->entityHandle().entity()).pixels.data(),
            glowBitmapBefore)
      << "glow bitmap was reallocated on the post-remap drag frame — fast path broke";
}

// Structural remap sanity: reparse the SAME bytes, build a remap between
// the two resulting documents, and assert every element is accounted for.
// The trees must be byte-identical so the remap covers every entity with
// RIC.
TEST_F(CompositorGoldenTest, BuildStructuralEntityRemapIdenticalTrees) {
  const char* kSource = R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
  <defs><filter id="blur"><feGaussianBlur in="SourceGraphic" stdDeviation="3"/></filter></defs>
  <rect width="200" height="100" fill="white"/>
  <g id="glow" filter="url(#blur)"><circle cx="150" cy="50" r="20" fill="yellow"/></g>
  <rect id="target" x="10" y="10" width="50" height="50" fill="red"/>
</svg>
  )svg";
  SVGDocument docA = parseDocument(kSource);
  SVGDocument docB = parseDocument(kSource);

  const auto remap = BuildStructuralEntityRemap(docA, docB);
  EXPECT_FALSE(remap.empty())
      << "BuildStructuralEntityRemap returned empty for byte-identical documents";

  // Same-doc trivial lookup: every element should have a remap entry.
  auto targetInA = docA.querySelector("#target");
  auto targetInB = docB.querySelector("#target");
  ASSERT_TRUE(targetInA.has_value());
  ASSERT_TRUE(targetInB.has_value());
  const Entity oldTarget = targetInA->entityHandle().entity();
  ASSERT_TRUE(remap.contains(oldTarget)) << "#target is missing from remap";
  EXPECT_EQ(remap.at(oldTarget), targetInB->entityHandle().entity())
      << "#target in docA doesn't map to #target in docB";
}

// Mismatched tree shape (docA has a rect, docB doesn't) → remap is empty
// and the compositor must fall back to `resetAllLayers`.
TEST_F(CompositorGoldenTest, BuildStructuralEntityRemapMismatchReturnsEmpty) {
  SVGDocument docA = parseDocument(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
  <rect width="50" height="50" fill="red"/>
  <rect width="50" height="50" fill="blue"/>
</svg>
  )svg");
  SVGDocument docB = parseDocument(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
  <rect width="50" height="50" fill="red"/>
</svg>
  )svg");

  const auto remap = BuildStructuralEntityRemap(docA, docB);
  EXPECT_TRUE(remap.empty())
      << "remap must be empty when child counts differ";
}

// Mismatched `id` → empty remap.
TEST_F(CompositorGoldenTest, BuildStructuralEntityRemapIdChangeReturnsEmpty) {
  SVGDocument docA = parseDocument(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
  <rect id="a" width="50" height="50" fill="red"/>
</svg>
  )svg");
  SVGDocument docB = parseDocument(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
  <rect id="b" width="50" height="50" fill="red"/>
</svg>
  )svg");

  const auto remap = BuildStructuralEntityRemap(docA, docB);
  EXPECT_TRUE(remap.empty()) << "id rename must break structural equivalence";
}

// Transform-attribute-only change → remap IS valid. This mirrors the
// drag-end writeback case: the source text changes only in `transform`
// attribute values; structurally the tree is unchanged.
TEST_F(CompositorGoldenTest, BuildStructuralEntityRemapIgnoresAttributeValueChanges) {
  SVGDocument docA = parseDocument(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
  <rect id="drag" x="10" y="10" width="40" height="40" fill="red" transform="translate(0,0)"/>
</svg>
  )svg");
  SVGDocument docB = parseDocument(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
  <rect id="drag" x="10" y="10" width="40" height="40" fill="red" transform="translate(30,0)"/>
</svg>
  )svg");

  const auto remap = BuildStructuralEntityRemap(docA, docB);
  EXPECT_FALSE(remap.empty())
      << "attribute-value-only change must leave the structural remap intact";
  auto dragA = docA.querySelector("#drag");
  auto dragB = docB.querySelector("#drag");
  ASSERT_TRUE(dragA.has_value() && dragB.has_value());
  EXPECT_EQ(remap.at(dragA->entityHandle().entity()),
            dragB->entityHandle().entity());
}

// Incremental GL-upload discipline: on the first click-to-drag after
// a cold render of the splash, the editor should observe AT MOST 3
// compositor tiles advance their `generation` counter — the two halves
// of the split segment and the new drag-target layer. Every other tile
// (filter-group layers, segments outside the split range, the root
// background) keeps its generation and the editor's GL texture for
// that slot stays bound. This is the key correctness property behind
// the "fluid first-drag-frame" contract.
TEST_F(CompositorGoldenTest, ClickToDragAdvancesAtMostThreeTileGenerations) {
  // Splash shape with the usual pattern: background rect, filter
  // group before the letters, letters, filter groups after the
  // letters. Mandatory-promote marks all four filter groups as
  // independent layers during cold-render eager-warmup.
  SVGDocument document = parseDocument(R"svg(
<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" width="892" height="512" viewBox="0 0 892 512">
  <defs>
    <filter id="b1"><feGaussianBlur in="SourceGraphic" stdDeviation="4"/></filter>
    <filter id="b2"><feGaussianBlur in="SourceGraphic" stdDeviation="5"/></filter>
    <filter id="b3"><feGaussianBlur in="SourceGraphic" stdDeviation="6"/></filter>
    <filter id="b4"><feGaussianBlur in="SourceGraphic" stdDeviation="7"/></filter>
  </defs>
  <rect width="892" height="512" fill="#0d0f1d"/>
  <g filter="url(#b1)"><rect x="50"  y="50" width="60" height="60" fill="yellow"/></g>
  <g filter="url(#b2)"><rect x="200" y="50" width="60" height="60" fill="yellow"/></g>
  <rect id="letter" x="400" y="100" width="80" height="120" fill="red"/>
  <g filter="url(#b3)"><rect x="600" y="50" width="60" height="60" fill="yellow"/></g>
  <g filter="url(#b4)"><rect x="750" y="50" width="60" height="60" fill="yellow"/></g>
</svg>
  )svg");
  auto target = document.querySelector("#letter");
  ASSERT_TRUE(target.has_value());

  RenderViewport vp;
  vp.size = Vector2d(892, 512);
  vp.devicePixelRatio = 1.0;

  CompositorController compositor(document, renderer_);
  compositor.setSkipMainComposeDuringSplit(true);

  // Phase 0 — cold render: detectors run, 4 filter groups promote,
  // eager-warmup rasterizes their layer bitmaps + 5 static segments
  // between/around them. No drag target yet.
  compositor.renderFrame(vp);

  // Snapshot the baseline generation of every tile AFTER cold render.
  const auto baselineTiles = compositor.snapshotTilesForUpload();
  std::unordered_map<uint64_t, uint64_t> baselineGen;
  for (const auto& tile : baselineTiles) {
    baselineGen[tile.tileId] = tile.generation;
  }

  // Phase 1 — click + drag in one frame: the editor sets the drag
  // target's transform and posts a render with the letter as the
  // drag entity.
  ASSERT_TRUE(compositor.promoteEntity(target->entityHandle().entity()));
  target->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(Vector2d(10.0, 0.0)));
  compositor.renderFrame(vp);

  const auto clickTiles = compositor.snapshotTilesForUpload();

  // Count tiles whose generation advanced since the cold baseline.
  // Net-new tiles (no entry in baseline) also count as "changed".
  int changedCount = 0;
  std::vector<std::string> changedNames;
  for (const auto& tile : clickTiles) {
    const auto it = baselineGen.find(tile.tileId);
    const bool isNewTile = (it == baselineGen.end());
    const bool generationAdvanced = !isNewTile && tile.generation != it->second;
    if (isNewTile || generationAdvanced) {
      ++changedCount;
      std::ostringstream label;
      label << "tile=" << tile.tileId << " gen=" << tile.generation
            << " isNew=" << (isNewTile ? "yes" : "no");
      changedNames.push_back(label.str());
    }
  }

  std::cerr << "[TILES] Click-to-drag advanced " << changedCount
            << " tile generation(s).\n";
  for (const auto& name : changedNames) {
    std::cerr << "        " << name << "\n";
  }

  // Hard invariant: click-to-drag must advance AT MOST 3 tile
  // generations — two halves of the split segment and the new
  // drag-target layer. If this trips, something in the compositor
  // is re-rasterizing a tile that didn't structurally change (a
  // filter-group layer that survived, a segment outside the split
  // range), which defeats the incremental GL-upload discipline.
  EXPECT_LE(changedCount, 3)
      << "click-to-drag advanced more than 3 tile generations — compositor is over-invalidating";
}

}  // namespace donner::svg::compositor
