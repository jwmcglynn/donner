#include <gmock/gmock.h>
#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string_view>

#include "donner/base/tests/Runfiles.h"
#include "donner/svg/SVGImageElement.h"
#include "donner/svg/renderer/PixelFormatUtils.h"
#include "donner/svg/renderer/RendererImageIO.h"
#include "donner/svg/renderer/tests/ImageComparisonTestFixture.h"
#include "donner/svg/tests/ParserTestUtils.h"

namespace donner::svg {
namespace {

using Params = ImageComparisonParams;

std::filesystem::path ResvgResourceRoot() {
  return Runfiles::instance().Rlocation("third_party/resvg-test-suite/");
}

/// A 2x2 solid red PNG as a data URI.
constexpr std::string_view kRedImageDataUri =
    "data:image/png;base64,"
    "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAAEElEQVR4nGP4z8AARAwQCgAf7gP9i18U1AAAAABJRU5E"
    "rkJggg==";

/// A 2x2 solid blue PNG as a data URI. @see kRedImageDataUri
constexpr std::string_view kBlueImageDataUri =
    "data:image/png;base64,"
    "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAAD0lEQVR4nGNgYPgPRmAKABf2A/1+6zfzAAAAAElFTkSu"
    "QmCC";

ImageComparisonParams GoldenParams() {
  Params params;
  params.enableGoldenUpdateFromEnv();
  return params;
}

/// Uses the existing pixelmatch assertion to reject a vacuous empty-bitmap identity result.
void ExpectVisibleBitmap(const RendererBitmap& bitmap, std::string_view label) {
  RendererBitmap empty = bitmap;
  std::fill(empty.pixels.begin(), empty.pixels.end(), 0);
  testing::TestPartResultArray differences;
  {
    testing::ScopedFakeTestPartResultReporter capture(
        testing::ScopedFakeTestPartResultReporter::INTERCEPT_ONLY_CURRENT_THREAD, &differences);
    ExpectBitmapsIdentical(bitmap, empty, label);
  }
  EXPECT_THAT(differences.size(), testing::Eq(1)) << "Expected visible rendered content";
}

class RendererRegressionTests : public ImageComparisonTestFixture {};

TEST_F(RendererRegressionTests, FontPropertiesDoNotChangeReducedCrosshair) {
  SVGDocument reduced = instantiateSubtree(R"(
    <svg width="500" height="500" viewBox="0 0 200 200">
      <path d="M 20 100 L 180 100 M 100 20 L 100 180" stroke="gray" stroke-width="0.5"/>
    </svg>
  )",
                                           {}, Vector2i(500, 500));
  SVGDocument styled = instantiateSubtree(R"(
    <svg width="500" height="500" viewBox="0 0 200 200"
         style="font:italic bold 200px serif; font-kerning:none; font-size-adjust:0.3">
      <path d="M 20 100 L 180 100 M 100 20 L 100 180" stroke="gray" stroke-width="0.5"/>
    </svg>
  )",
                                          {}, Vector2i(500, 500));
  const auto pathOnly = RenderDocumentWithBackend(reduced, RendererBackend::TinySkia);
  const auto withFonts = RenderDocumentWithBackend(styled, RendererBackend::TinySkia);
  ASSERT_THAT(pathOnly.empty(), testing::IsFalse());
  ASSERT_THAT(pathOnly.dimensions, testing::Eq(Vector2i(500, 500)));
  ExpectVisibleBitmap(pathOnly, "crosshair_visible");
  ExpectBitmapsIdentical(withFonts, pathOnly, "font_properties_path_independence");
  std::cout << "Reduced crosshair alpha at (249,100)/(250,100): "
            << static_cast<int>(pathOnly.pixels[100 * pathOnly.rowBytes + 249 * 4 + 3]) << "/"
            << static_cast<int>(pathOnly.pixels[100 * pathOnly.rowBytes + 250 * 4 + 3]) << "\n";
  if (const char* output = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR")) {
    const auto straight = pathOnly.alphaType == AlphaType::Premultiplied
                              ? UnpremultiplyRgbaRows(pathOnly.pixels, 500, 500, pathOnly.rowBytes)
                              : CopyTightRgbaRows(pathOnly.pixels, 500, 500, pathOnly.rowBytes);
    const auto image = std::filesystem::path(output) / "font_crosshair_path_only.png";
    ASSERT_THAT(
        RendererImageIO::writeRgbaPixelsToPngFile(image.string().c_str(), straight, 500, 500),
        testing::IsTrue());
  }
}

TEST_F(RendererRegressionTests, FontSizeAdjustMatchesExplicitUsedSize) {
  SVGDocument adjusted = instantiateSubtree(R"(
    <svg viewBox="0 0 200 200" font-family="Noto Sans" font-size="64">
      <text x="100" y="100" text-anchor="middle" font-size-adjust="0.3">Text</text>
    </svg>
  )",
                                            {}, Vector2i(500, 500));
  SVGDocument explicitSize = instantiateSubtree(R"(
    <svg viewBox="0 0 200 200" font-family="Noto Sans" font-size="35.82089552238806">
      <text x="100" y="100" text-anchor="middle">Text</text>
    </svg>
  )",
                                                {}, Vector2i(500, 500));
  RegisterFontsFromDirectoryForTesting(adjusted, ResvgResourceRoot() / "fonts");
  RegisterFontsFromDirectoryForTesting(explicitSize, ResvgResourceRoot() / "fonts");
  const RendererBitmap actual = RenderDocumentWithBackend(adjusted, ActiveRendererBackend());
  const RendererBitmap expected = RenderDocumentWithBackend(explicitSize, ActiveRendererBackend());
  ASSERT_THAT(actual.empty(), testing::IsFalse());
  ASSERT_THAT(expected.empty(), testing::IsFalse());
  ASSERT_THAT(actual.dimensions, testing::Eq(Vector2i(500, 500)));
  ExpectVisibleBitmap(actual, "font_size_adjust_visible");
  // Noto Sans has 1000 units per em and an OS/2 x-height of 536: 64 * 0.3 / 0.536.
  ExpectBitmapsIdentical(actual, expected, "font_size_adjust_used_size");
}

TEST_F(RendererRegressionTests, FontSizeAdjustDecorationMatchesExplicitDeclaringSize) {
  for (const std::string decoration : {"underline", "overline", "line-through"}) {
    for (bool childOverride : {false, true}) {
      SCOPED_TRACE(decoration);
      SCOPED_TRACE(childOverride);
      const std::string content =
          childOverride
              ? "<tspan font-family='MPLUS 1p' font-size='20' font-size-adjust='none'>Text</tspan>"
              : "Text";
      const std::string start =
          "<svg viewBox='0 0 200 200' font-family='Noto Sans'><text x='30' y='100' "
          "text-decoration='" +
          decoration + "' ";
      SVGDocument adjusted = instantiateSubtree(
          start + "font-size='64' font-size-adjust='0.3'>" + content + "</text></svg>", {},
          Vector2i(500, 500));
      SVGDocument explicitSize =
          instantiateSubtree(start + "font-size='35.82089552238806'>" + content + "</text></svg>",
                             {}, Vector2i(500, 500));
      RegisterFontsFromDirectoryForTesting(adjusted, ResvgResourceRoot() / "fonts");
      RegisterFontsFromDirectoryForTesting(explicitSize, ResvgResourceRoot() / "fonts");
      const RendererBitmap actual = RenderDocumentWithBackend(adjusted, ActiveRendererBackend());
      const RendererBitmap expected =
          RenderDocumentWithBackend(explicitSize, ActiveRendererBackend());
      ASSERT_THAT(actual.empty(), testing::IsFalse());
      ASSERT_THAT(expected.empty(), testing::IsFalse());
      const std::string label =
          "adjusted_decoration_" + decoration + (childOverride ? "_child" : "_self");
      ExpectVisibleBitmap(actual, label + "_visible");
      ExpectBitmapsIdentical(actual, expected, label);
    }
  }
}

TEST_F(RendererRegressionTests, FontShorthandMatchesExpandedLonghands) {
  SVGDocument shorthand = instantiateSubtree(R"(
    <svg viewBox="0 0 200 200">
      <g style="font: italic bold 200px serif; font-kerning: none; font-size-adjust: 0.3">
        <text x="55" y="100" style="font: 50px 'Noto Sans'">AVA</text>
      </g>
    </svg>
  )",
                                             {}, Vector2i(500, 500));
  SVGDocument expanded = instantiateSubtree(R"(
    <svg viewBox="0 0 200 200">
      <text x="55" y="100" style="font-size:50px; font-family:'Noto Sans';
          font-style:normal; font-weight:400; font-kerning:auto; font-size-adjust:none">AVA</text>
    </svg>
  )",
                                            {}, Vector2i(500, 500));
  RegisterFontsFromDirectoryForTesting(shorthand, ResvgResourceRoot() / "fonts");
  RegisterFontsFromDirectoryForTesting(expanded, ResvgResourceRoot() / "fonts");
  const RendererBitmap actual = RenderDocumentWithBackend(shorthand, ActiveRendererBackend());
  const RendererBitmap expected = RenderDocumentWithBackend(expanded, ActiveRendererBackend());
  ASSERT_THAT(actual.empty(), testing::IsFalse());
  ASSERT_THAT(expected.empty(), testing::IsFalse());
  ASSERT_THAT(actual.dimensions, testing::Eq(Vector2i(500, 500)));
  ExpectVisibleBitmap(actual, "font_shorthand_visible");
  ExpectBitmapsIdentical(actual, expected, "font_shorthand_expansion");
}

TEST_F(RendererRegressionTests, MarkerPercentResolvesAgainstReferencingViewport) {
  const char* svg = "donner/svg/renderer/testdata/marker_percent_nested_viewport.svg";
  const char* golden = "donner/svg/renderer/testdata/golden/marker_percent_nested_viewport.png";

  SVGDocument document = loadSVG(svg, ResvgResourceRoot());
  renderAndCompare(document, svg, golden, GoldenParams());
}

TEST_F(RendererRegressionTests, TextDecorationUnderlineRenders) {
  const char* svg = "donner/svg/renderer/testdata/geode_text_decoration_underline.svg";
  const char* golden = "donner/svg/renderer/testdata/golden/geode_text_decoration_underline.png";

  SVGDocument document = loadSVG(svg, ResvgResourceRoot());
  renderAndCompare(document, svg, golden, GoldenParams());
}

TEST_F(RendererRegressionTests, InlineSizeAutoFlowWrapsText) {
  // SVG2 inline-size: text greedily wraps to the 150px measure into stacked lines.
  const char* svg = "donner/svg/renderer/testdata/text_inline_size_wrap.svg";
  const char* golden = "donner/svg/renderer/testdata/golden/text_inline_size_wrap.png";

  SVGDocument document = loadSVG(svg, ResvgResourceRoot());
  Params params = GoldenParams();
  params.withGeodeMaxPixelsDifferent(160);
  renderAndCompare(document, svg, golden, params);
}

TEST_F(RendererRegressionTests, PatternFillOnTextDoesNotLeakToNextShape) {
  const char* svg = "donner/svg/renderer/testdata/geode_text_pattern_fill.svg";
  const char* golden = "donner/svg/renderer/testdata/golden/geode_text_pattern_fill.png";

  SVGDocument document = loadSVG(svg, ResvgResourceRoot());
  renderAndCompare(document, svg, golden, GoldenParams());
}

TEST_F(RendererRegressionTests, SpanGradientOverridesElementPatternFill) {
  const char* svg = "donner/svg/renderer/testdata/geode_text_span_gradient_over_pattern.svg";
  const char* golden =
      "donner/svg/renderer/testdata/golden/geode_text_span_gradient_over_pattern.png";

  SVGDocument document = loadSVG(svg, ResvgResourceRoot());
  renderAndCompare(document, svg, golden, GoldenParams());
}

TEST_F(RendererRegressionTests, SpanGradientOverridesElementPatternStroke) {
  const char* svg = "donner/svg/renderer/testdata/geode_text_span_gradient_over_pattern_stroke.svg";
  const char* golden =
      "donner/svg/renderer/testdata/golden/geode_text_span_gradient_over_pattern_stroke.png";

  SVGDocument document = loadSVG(svg, ResvgResourceRoot());
  renderAndCompare(document, svg, golden, GoldenParams());
}

TEST_F(RendererRegressionTests, NestedBaselineShiftRedrawIsIdempotent) {
  const char* svg = "donner/svg/renderer/testdata/text_nested_baseline_shift_idempotency.svg";
  SVGDocument document = loadSVG(svg, ResvgResourceRoot());

  const RendererBitmap first = RenderDocumentWithBackend(document, RendererBackend::TinySkia);
  const RendererBitmap second = RenderDocumentWithBackend(document, RendererBackend::TinySkia);

  ASSERT_FALSE(first.empty());
  ASSERT_FALSE(second.empty());
  ExpectBitmapsIdentical(second, first, "nested_baseline_shift_redraw");
}

TEST_F(RendererRegressionTests, FeImageFragmentRedrawIsIdempotent) {
  const char* svg = "donner/svg/renderer/testdata/feimage_fragment_idempotency.svg";
  SVGDocument document = loadSVG(svg, ResvgResourceRoot());

  const RendererBitmap first = RenderDocumentWithBackend(document, RendererBackend::TinySkia);
  const RendererBitmap second = RenderDocumentWithBackend(document, RendererBackend::TinySkia);

  ASSERT_FALSE(first.empty());
  ASSERT_FALSE(second.empty());
  ExpectBitmapsIdentical(second, first, "feimage_fragment_redraw");
}

// vector-effect: non-scaling-stroke keeps the stroke a constant device width under a scaled
// viewBox and an additional transform. The golden captures the correct rendering: thin (2px) blue
// non-scaling strokes next to thick (8px / 16px) red strokes that scale with the transform.
TEST_F(RendererRegressionTests, VectorEffectNonScalingStrokeIsConstantWidth) {
  const char* svg = "donner/svg/renderer/testdata/vector_effect_non_scaling_stroke.svg";
  const char* golden = "donner/svg/renderer/testdata/golden/vector_effect_non_scaling_stroke.png";

  SVGDocument document = loadSVG(svg, ResvgResourceRoot());
  renderAndCompare(document, svg, golden, GoldenParams());
}

// Self-validating guard against a silent no-op: the same document with and without the
// vector-effect attribute must render differently. If non-scaling-stroke were ignored, the two
// renders would be identical.
TEST_F(RendererRegressionTests, VectorEffectNonScalingStrokeChangesOutput) {
  const char* nonScalingSvg = "donner/svg/renderer/testdata/vector_effect_non_scaling_stroke.svg";
  const char* controlSvg = "donner/svg/renderer/testdata/vector_effect_scaling_stroke_control.svg";

  SVGDocument nonScalingDoc = loadSVG(nonScalingSvg, ResvgResourceRoot());
  SVGDocument controlDoc = loadSVG(controlSvg, ResvgResourceRoot());

  const RendererBitmap nonScaling =
      RenderDocumentWithBackend(nonScalingDoc, RendererBackend::TinySkia);
  const RendererBitmap control = RenderDocumentWithBackend(controlDoc, RendererBackend::TinySkia);

  ASSERT_FALSE(nonScaling.empty());
  ASSERT_FALSE(control.empty());
  ASSERT_EQ(nonScaling.dimensions, control.dimensions);
  ASSERT_EQ(nonScaling.pixels.size(), control.pixels.size());
  EXPECT_NE(nonScaling.pixels, control.pixels)
      << "vector-effect: non-scaling-stroke had no effect on the rendered output";
}

// The tiny-skia backend memoizes each shape's converted `tiny_skia::Path` on the shape's source
// entity, so a geometry change has to drop that entry before the next draw reads it. The caches
// live on the document, not on the renderer, so the control render has to come from a separately
// parsed document that has no cache entries at all - a second renderer over the same document
// would read the same (possibly stale) entry and agree with a wrong answer.
TEST_F(RendererRegressionTests, PathMutationInvalidatesTinySkiaConvertedPathCache) {
  SVGDocument document = instantiateSubtree(
      R"(<path id="p" d="M 0 0 L 4 0 L 4 4 Z" fill="black"/>)", {}, Vector2i(16, 16));

  std::unique_ptr<RendererInterface> renderer = CreateRendererInstance(RendererBackend::TinySkia);
  ASSERT_NE(renderer, nullptr);
  renderer->draw(document);
  const RendererBitmap beforeMutation = renderer->takeSnapshot();
  ASSERT_FALSE(beforeMutation.empty());

  auto path = document.querySelector("#p");
  ASSERT_TRUE(path.has_value());
  path->setAttribute("d", "M 0 0 L 16 0 L 16 16 Z");

  renderer->draw(document);
  const RendererBitmap afterMutation = renderer->takeSnapshot();
  ASSERT_FALSE(afterMutation.empty());
  // Guards against a vacuous pass: if the mutation were invisible, a stale cache would be
  // indistinguishable from a correctly invalidated one.
  ASSERT_NE(afterMutation.pixels, beforeMutation.pixels)
      << "the `d` mutation must change the rendering for this test to detect a stale conversion";

  SVGDocument freshDocument = instantiateSubtree(
      R"(<path id="p" d="M 0 0 L 16 0 L 16 16 Z" fill="black"/>)", {}, Vector2i(16, 16));
  const RendererBitmap fresh = RenderDocumentWithBackend(freshDocument, RendererBackend::TinySkia);
  ASSERT_FALSE(fresh.empty());
  ExpectBitmapsIdentical(afterMutation, fresh, "tiny_skia_path_cache_invalidation");
}

// Same contract for the premultiplied-image cache, which is keyed by the element that loaded the
// pixels: replacing the `href` drops the loaded image, and the cached conversion has to go with it.
TEST_F(RendererRegressionTests, ImageHrefChangeInvalidatesTinySkiaPremultipliedImageCache) {
  const std::string redMarkup =
      std::string(R"(<image id="i" x="0" y="0" width="16" height="16" href=")") +
      std::string(kRedImageDataUri) + R"(" />)";
  const std::string blueMarkup =
      std::string(R"(<image id="i" x="0" y="0" width="16" height="16" href=")") +
      std::string(kBlueImageDataUri) + R"(" />)";

  auto fragment = instantiateSubtreeElementAs<SVGImageElement>(redMarkup, {}, Vector2i(16, 16));

  std::unique_ptr<RendererInterface> renderer = CreateRendererInstance(RendererBackend::TinySkia);
  ASSERT_NE(renderer, nullptr);
  renderer->draw(fragment.document);
  const RendererBitmap beforeMutation = renderer->takeSnapshot();
  ASSERT_FALSE(beforeMutation.empty());

  fragment->setHref(RcString(kBlueImageDataUri));

  renderer->draw(fragment.document);
  const RendererBitmap afterMutation = renderer->takeSnapshot();
  ASSERT_FALSE(afterMutation.empty());
  ASSERT_NE(afterMutation.pixels, beforeMutation.pixels)
      << "the `href` mutation must change the rendering for this test to detect a stale conversion";

  auto freshFragment =
      instantiateSubtreeElementAs<SVGImageElement>(blueMarkup, {}, Vector2i(16, 16));
  const RendererBitmap fresh =
      RenderDocumentWithBackend(freshFragment.document, RendererBackend::TinySkia);
  ASSERT_FALSE(fresh.empty());
  ExpectBitmapsIdentical(afterMutation, fresh, "tiny_skia_image_cache_invalidation");
}

}  // namespace
}  // namespace donner::svg
