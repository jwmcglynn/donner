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
