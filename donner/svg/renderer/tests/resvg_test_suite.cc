#include <gmock/gmock.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "donner/base/tests/Runfiles.h"
#include "donner/svg/renderer/tests/ImageComparisonTestFixture.h"
#include "donner/svg/renderer/tests/RendererTestBackend.h"

using testing::Combine;
using testing::ValuesIn;

namespace donner::svg {

using Params = ImageComparisonParams;

namespace {

ImageComparisonParams WithMaxPixels(int maxMismatchedPixels, std::string_view reason) {
  ImageComparisonParams params;
  params.withMaxPixelsDifferent(maxMismatchedPixels).withReason(reason);
  return params;
}

ImageComparisonParams categoryFeatureRequirements(std::string_view category) {
  ImageComparisonParams params;
  if (category.rfind("filters/", 0) == 0) {
    params.requireFeature(RendererBackendFeature::FilterEffects, "filter effects");
  }
  if (category.rfind("text/", 0) == 0) {
    params.requireFeature(RendererBackendFeature::Text, "text rendering");
  }
  return params;
}

void applyCommonFeatureRequirements(ImageComparisonParams& params,
                                    const ImageComparisonParams& requirements) {
  const bool hadRequirements = params.requiredFeatures != 0u;
  params.requiredFeatures |= requirements.requiredFeatures;
  if (!hadRequirements && params.backendRequirementReason.empty()) {
    params.backendRequirementReason = requirements.backendRequirementReason;
  }
}

// Discover every .svg test in one category directory under the resvg-test-suite
// tree. Example: getTestsInCategory("painting/fill") -> all .svg files in
// <runfiles>/resvg-test-suite/tests/painting/fill/.
//
// Overrides is keyed by the bare filename (e.g. "rgb-int-int-int.svg") and
// picks per-test params (Skip, threshold, golden override, etc). Any file not
// in the overrides map uses defaultParams. Category-wide feature requirements
// are additive so per-test overrides do not drop required backend support.
std::vector<ImageComparisonTestcase> getTestsInCategory(
    std::string_view category, std::map<std::string, ImageComparisonParams> overrides = {},
    ImageComparisonParams defaultParams = {}) {
  const std::string kTestsRoot =
      Runfiles::instance().Rlocation("third_party/resvg-test-suite/tests");
  const std::filesystem::path kCategoryDir = std::filesystem::path(kTestsRoot) / category;

  std::vector<ImageComparisonTestcase> testPlan;
  if (!std::filesystem::exists(kCategoryDir)) {
    return testPlan;
  }

  const ImageComparisonParams commonRequirements = categoryFeatureRequirements(category);

  for (const auto& entry : std::filesystem::directory_iterator(kCategoryDir)) {
    if (entry.path().extension() != ".svg") {
      continue;
    }
    const std::string filename = entry.path().filename().string();
    ImageComparisonTestcase test;
    test.svgFilename = entry.path();
    test.params = defaultParams;

    if (auto it = overrides.find(filename); it != overrides.end()) {
      test.params = it->second;
    }
    applyCommonFeatureRequirements(test.params, commonRequirements);

    // Unless the case sets one, its canvas is the one its reference image was rendered on, read
    // from that image when the case runs. A render-only case compares with no image, so it keeps
    // the suite's 500x500 canvas.
    if (!test.params.canvasSize && test.params.isRenderOnly()) {
      test.params.setCanvasSize(500, 500);
    }
    test.params.enableCanvasSizeFromReference();

    testPlan.emplace_back(std::move(test));
  }

  std::sort(testPlan.begin(), testPlan.end());
  return testPlan;
}

/// Params for a text row whose Geode comparison against its vendored resvg reference exceeds its
/// budget only because of the reference's quarter-pixel edge quantization (see
/// README_resvg_test_suite.md); pixels the rule does not accept still count against the unchanged
/// budget. Listed in QUARTER_PIXEL_EDGE_ALLOWLIST in tools/resvg_parity/parity_report.py.
ImageComparisonParams QuarterPixelEdgeReference() {
  return Params().withGeodeQuarterPixelEdgeCoverage().withReason(
      "Reference edge quantization: the PNG places edges on quarter-pixel rows and Geode renders "
      "exact coverage");
}

/// \p params, with the Geode comparison disabled: Geode's edge coverage differs from the reference
/// on stroke and decoration edges, mostly at composite pixels the quarter-pixel edge coverage rule
/// refuses, and part of the difference exceeds what quarter-pixel quantization allows, so
/// quantization alone is not shown to explain it.
ImageComparisonParams GeodeUnresolvedEdgeCoverageGate(ImageComparisonParams params) {
  return params.disableBackend(
      RendererBackend::Geode,
      "Unresolved: with the edge coverage rule Geode counts 117 px (marker text child), 162 "
      "(tspan-decoration) and 136 (underline-with-dy-list-2), mostly composite pixels on stroke "
      "and decoration edges; 16, 80 and 1 of them differ by more than quarter-pixel "
      "quantization allows");
}

/// \p params, with the Geode comparison disabled: an edge lies under a translucent layer, so the
/// reference's quarter-pixel edge coverage changes the composite's straight color, and the layer
/// covers the rows beside the edge partially, which the quarter-pixel edge coverage rule counts.
/// Part of the difference exceeds what quantization allows, so it is not shown to be the only
/// cause.
ImageComparisonParams GeodeTranslucentEdgeColorGate(ImageComparisonParams params) {
  return params.disableBackend(
      RendererBackend::Geode,
      "Unresolved: the crosshair's quarter-pixel edge coverage under the half-transparent masked "
      "text changes the composite color and lies beside partially covered rows (109 of the 120 "
      "px Geode counts with the edge coverage rule, against a CPU count of 19), but 42 of the 120 "
      "differ by more than quarter-pixel quantization allows");
}

/// \p params, with the Geode comparison disabled: the row compares against a custom golden that is
/// not quarter-pixel quantized (its frame is an exact area average), so the quarter-pixel edge
/// coverage rule does not apply, and Geode's edge coverage differs from it by more than the pixel
/// budget.
ImageComparisonParams GeodeUnquantizedGoldenGate(ImageComparisonParams params) {
  return params.disableBackend(
      RendererBackend::Geode,
      "Harness mismatch: this custom golden is not quarter-pixel quantized, so the edge coverage "
      "rule does not apply, and Geode's exact edge coverage differs from it beyond the pixel "
      "budget");
}

/// \p params, with the Geode comparison disabled: the row blurs text over the crosshair and
/// compares against a Donner software rendering. The golden's quarter-pixel quantized crosshair
/// rows lie beside the partially covered blurred text, which the quarter-pixel edge coverage rule
/// counts.
ImageComparisonParams GeodeBlurredCrosshairGate(ImageComparisonParams params) {
  return params.disableBackend(
      RendererBackend::Geode,
      "Harness mismatch: the golden's quarter-pixel quantized crosshair rows lie beside the "
      "partially covered blurred text, which the edge coverage rule counts (602 px with the rule, "
      "all on the crosshair's two rows)");
}

/// \p params, with the Geode comparison disabled: the row blurs text on paths and compares against
/// a Donner software rendering that is not quarter-pixel quantized there, so quantization does not
/// explain the difference.
ImageComparisonParams GeodeUnresolvedBlurGate(ImageComparisonParams params) {
  return params.disableBackend(
      RendererBackend::Geode,
      "Unresolved: Geode counts 496 px (497 on software Vulkan) against this Donner software "
      "rendering, which the CPU renderer matches exactly; the golden is not quarter-pixel "
      "quantized there (share 0.011), and the pixels lie on the blurred glyph edges, the guide "
      "paths and their overlap");
}

/// \p params, with the Geode comparison disabled: the row's CPU comparison has a pixel budget above
/// the default, and that budget is not approved for the Geode lane (PIXEL_BUDGET_ALLOWLIST has no
/// Geode entry for it).
ImageComparisonParams GeodeCpuAllowanceGate(ImageComparisonParams params) {
  return params.disableBackend(
      RendererBackend::Geode,
      "Harness mismatch: the 1200 px CPU budget for font-variant is not approved for Geode, and "
      "comparing would apply a budget in the hundreds to it; Geode differs by 1108 px");
}

/// Params for a drop-shadow() row compared against the Donner golden at \p path for \p reason
/// (see testdata/golden/README.md). The params keep views of both, so they must be string literals.
ImageComparisonParams DropShadowFunctionGolden(std::string_view path, std::string_view reason) {
  return Params::WithGoldenOverride(path, kDefaultThreshold, reason);
}

/// Params for the feColorMatrix identity rows, whose references posterize through 8-bit linearRGB;
/// they share one Donner golden (see testdata/golden/README.md).
ImageComparisonParams ColorMatrixIdentityGolden() {
  return Params::WithGoldenOverride(
      "donner/svg/renderer/testdata/golden/donner-feColorMatrix-identity.png", kDefaultThreshold,
      "Reference disagreement: identity output; the reference stores linearRGB in 8 bits");
}

/// Params for the blurred-text rows, compared against the Donner golden at \p path (see
/// testdata/golden/README.md): their PNGs draw every glyph edge outside the outline, and the blur
/// spreads that extra ink into the glyph interiors, where the comparison counts it. The params keep
/// a view of \p path, so it must be a string literal.
ImageComparisonParams HeavyGlyphReferenceGolden(std::string_view path) {
  return Params::WithGoldenOverride(
      path, kDefaultThreshold,
      "Reference disagreement: the PNG draws glyph edges 0.25 to 0.3 px outside the outline; "
      "Donner's glyph ink equals the outline's area (GlyphInkAreaTest)");
}

/// Params for a color emoji row compared against the Donner golden at \p path (see
/// testdata/golden/README.md): its resvg reference matches the glyphs and draws the 1.25 px
/// vertical guide 1/8 px right of its exact coverage. The params keep a view of \p path, so it must
/// be a string literal.
ImageComparisonParams EmojiGuideGolden(std::string_view path) {
  return Params::WithGoldenOverride(
             path, kDefaultThreshold,
             "Reference disagreement: the vertical guide is drawn 1/8 px right of its coverage")
      .onlyTextFull();
}

/// The CPU comparison of a gradient-stroked text row uses \p firefoxPath; the Geode comparison uses
/// \p geodePath, Geode's own rendering, so the Geode lane is a regression check against itself
/// while the CPU lane keeps the independent Firefox comparison. Firefox, like the CPU renderer,
/// samples stroke edges on sub-scanlines; Geode's coverage is closer to an area-coverage control
/// than either (golden README). Both path arguments refer to string literals.
ImageComparisonParams GradientStrokeOnTextGoldens(std::string_view firefoxPath,
                                                  std::string_view geodePath) {
  return Params::WithGoldenOverride(firefoxPath)
      .withReason(
          "Reference disagreement: paint mapped by glyph ink extents; Geode compares with its own "
          "rendering, whose coverage is closer to an area-coverage control than the Firefox "
          "golden's and the CPU renderer's (within 8 levels at 365 of 511 and 347 of 497 counted "
          "pixels, mean 6.1, against about 18)")
      .withGeodeGoldenOverride(geodePath)
      .requireFeature(RendererBackendFeature::Text, "text rendering");
}

/// These references preserve the vendored filter pixels. Only the independently outlined
/// labels use each backend's path coverage. Both path arguments refer to string literals.
ImageComparisonParams ColorAdjustReference(std::string_view cpuPath, std::string_view geodePath) {
  return Params::WithGoldenOverride(cpuPath, kDefaultThreshold,
                                    "Reference disagreement: font smoothing on labels; original "
                                    "filter pixels and per-backend coverage of usvg label outlines")
      .withGeodeGoldenOverride(geodePath);
}

}  // namespace

TEST_P(ImageComparisonTestFixture, ResvgTest) {
  const ImageComparisonTestcase& testcase = std::get<0>(GetParam());

  // In the post-rename layout goldens sit next to their .svg in the same
  // category directory, unless overridden by WithGoldenOverride().
  std::filesystem::path goldenFilename;
  if (testcase.params.overrideGoldenFilename.empty()) {
    goldenFilename = testcase.svgFilename;
    goldenFilename.replace_extension(".png");
  } else {
    goldenFilename = testcase.params.overrideGoldenFilename;
  }

  const auto resourceRoot = Runfiles::instance().Rlocation("third_party/resvg-test-suite/");
  SVGDocument document =
      loadSVG(testcase.svgFilename.string().c_str(), resourceRoot,
              CreateResvgResourceLoaderForTesting(
                  resourceRoot, testcase.svgFilename,
                  Runfiles::instance().Rlocation("donner/svg/renderer/testdata/http_resources")));
  renderAndCompare(document, testcase.svgFilename, goldenFilename.string().c_str());
}

INSTANTIATE_TEST_SUITE_P(
    FiltersEnableBackground, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "filters/enable-background", {},
                // The references take BackgroundImage from an enable-background snapshot, which
                // SVG 2 removed; Donner reads the Filter Effects isolation-group backdrop instead.
                Params::DeprecatedInSVG2("enable-background / BackgroundImage (SVG 1.1, removed "
                                         "in SVG 2)"))),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(FiltersFeBlend, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("filters/feBlend")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFeColorMatrix, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "filters/feColorMatrix",
                {
                    // Identity forms compare against a Donner golden: the references posterize dark
                    // tones through 8-bit linearRGB.
                    {"type=hueRotate-without-an-angle.svg", ColorMatrixIdentityGolden()},
                    {"type=matrix-with-empty-values.svg", ColorMatrixIdentityGolden()},
                    {"type=matrix-with-non-normalized-values.svg",
                     Params::WithThreshold(
                         0.03f, kDefaultMismatchedPixels,
                         "Archived green varies along a constant-color gradient column")},
                    {"type=matrix-with-not-enough-values.svg", ColorMatrixIdentityGolden()},
                    {"type=matrix-with-too-many-values.svg", ColorMatrixIdentityGolden()},
                    {"type=matrix-without-values.svg", ColorMatrixIdentityGolden()},
                    {"type=saturate-with-a-large-coefficient.svg",
                     Params::UndefinedBehavior("saturate 99999")},
                    {"type=saturate-with-negative-coefficient.svg",
                     Params::UndefinedBehavior("saturate -0.5")},
                    {"type=saturate-without-a-coefficient.svg", ColorMatrixIdentityGolden()},
                    {"type=saturate.svg",
                     Params::WithThreshold(
                         0.03f, kDefaultMismatchedPixels,
                         "Dark gradients cross an eight-bit linearRGB step in the reference")},
                    {"without-attributes.svg", ColorMatrixIdentityGolden()},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(FiltersFeComponentTransfer, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("filters/feComponentTransfer")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(FiltersFeComposite, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("filters/feComposite")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(FiltersFeConvolveMatrix, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory(
                                     "filters/feConvolveMatrix",
                                     {
                                         {"bias=-0.5.svg", Params::UndefinedBehavior("bias=-0.5")},
                                         {"bias=0.5.svg", Params::UndefinedBehavior("bias=0.5")},
                                         {"bias=9999.svg", Params::UndefinedBehavior("bias=9999")},
                                         {"edgeMode=wrap-with-matrix-larger-than-target.svg",
                                          Params::UndefinedBehavior("wrap with oversized kernel")},
                                     })),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFeDiffuseLighting, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "filters/feDiffuseLighting",
                {
                    {"complex-transform.svg",
                     Params::WithThreshold(0.11f, kDefaultMismatchedPixels,
                                           "Reference shades the alpha ramp before skew")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(FiltersFeDisplacementMap, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("filters/feDisplacementMap")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(FiltersFeDistantLight, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("filters/feDistantLight")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(FiltersFeDropShadow, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("filters/feDropShadow")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(FiltersFeFlood, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("filters/feFlood")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFeGaussianBlur, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "filters/feGaussianBlur",
                {
                    {"complex-transform.svg",
                     Params().withGeodeGoldenOverride(
                         "donner/svg/renderer/testdata/golden/geode/"
                         "filters_feGaussianBlur_complex-transform.png",
                         "The covered area of the rotated rect agrees with a 16x scan "
                         "control to <0.3 px in total. Coverage along its edges differs, "
                         "and the directional stdDeviation=\"12 0\" blur widens that into a "
                         "thin band (262 px on Metal, 247 on software Vulkan at the default "
                         "threshold). TinySkia differs by 99 px from the vendored reference.")},
                    {"huge-stdDeviation.svg",
                     Params::UndefinedBehavior(
                         "implementation-defined resource limit; blur sigma is "
                         "clamped to 256 device px (kMaximumFilterPixelRadius) and "
                         "the reference clamps differently")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(FiltersFeImage, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("filters/feImage")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFeMerge, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "filters/feMerge",
                {
                    {"complex-transform.svg",
                     Params::WithThreshold(0.09f, kDefaultMismatchedPixels,
                                           "Reference snaps the skewed offset to whole pixels")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(FiltersFeMorphology, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("filters/feMorphology")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(FiltersFeOffset, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("filters/feOffset")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFePointLight, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "filters/fePointLight",
                {
                    {"complex-transform.svg",
                     Params::WithThreshold(0.11f, kDefaultMismatchedPixels,
                                           "Reference evaluates the point light before skew")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(FiltersFeSpecularLighting, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("filters/feSpecularLighting")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFeSpotLight, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "filters/feSpotLight",
                {
                    {"complex-transform.svg",
                     Params::WithGoldenOverride(
                         "donner/svg/renderer/testdata/golden/resvg-complex-transform.png")
                         .withReason("Reference shades before skew and attenuates the cone "
                                     "edge differently")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFeTile, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory("filters/feTile",
                                        {
                                            {"complex-transform.svg",
                                             Params::UndefinedBehavior("complex transform")},
                                        })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFeTurbulence, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory("filters/feTurbulence",
                                        {
                                            {"stitchTiles=stitch.svg",
                                             Params::UndefinedBehavior("stitchTiles=stitch")},
                                        })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFilter, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "filters/filter",
            {
                // Donner-rendered golden; see donner/svg/renderer/testdata/golden/README.md.
                {"invalid-FuncIRI.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/donner-invalid-FuncIRI.png",
                     kDefaultThreshold,
                     "Reference disagreement: the PNG hides the rect; a url() naming no element "
                     "disables the whole filter chain, so the rect draws unfiltered "
                     "(FilterReferenceStages.InvalidReferenceDisablesEntireChain)")},
                {"everything-via-xlink-href.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-xlink-filter-bounds.png")
                     .withReason("Inherited filter bounds match the independently clipped input")},
                {"some-attributes-via-xlink-href.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-xlink-filter-bounds.png")
                     .withReason("Inherited filter bounds match the independently clipped input")},
                {"with-region-and-subregion.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-filter-input-and-subregion.png")
                     .withReason("Independent input clipping and primitive output subregion")},
                // Donner-rendered golden; see donner/svg/renderer/testdata/golden/README.md.
                {"complex-order-and-xlink-href.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/donner-complex-order-and-xlink-href.png",
                     kDefaultThreshold,
                     "Reference disagreement: resvg drops blur spill at the region edge; see "
                     "golden/README.md")},
                {"in=BackgroundAlpha-with-enable-background.svg",
                 Params::DeprecatedInSVG2("enable-background: BackgroundAlpha of an SVG 1.1 "
                                          "enable-background=new snapshot (removed in SVG 2)")},
                {"in=BackgroundImage-with-enable-background.svg",
                 Params::DeprecatedInSVG2("enable-background: BackgroundImage of an SVG 1.1 "
                                          "enable-background=new snapshot (removed in SVG 2)")},
                {"in=FillPaint-on-g-without-children.svg",
                 Params::UndefinedBehavior("in=FillPaint on empty group")},
                {"in=FillPaint-with-gradient.svg",
                 Params::UndefinedBehavior("in=FillPaint gradient")},
                {"in=FillPaint-with-pattern.svg",
                 Params::UndefinedBehavior("in=FillPaint pattern")},
                {"in=FillPaint-with-target-on-g.svg",
                 Params::UndefinedBehavior("in=FillPaint on group")},
                {"in=FillPaint.svg", Params::UndefinedBehavior("in=FillPaint")},
                {"in=StrokePaint.svg", Params::UndefinedBehavior("in=StrokePaint")},
                {"on-the-root-svg.svg", Params::UndefinedBehavior("Filter on the root `svg`")},
                {"subregion-and-primitiveUnits=objectBoundingBox-1.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-half-box-blur.png",
                     kDefaultThreshold,
                     "Explicit half-box clip; opaque interior and exact crosshair")},
                {"subregion-and-primitiveUnits=objectBoundingBox-2.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-half-box-blur.png",
                     kDefaultThreshold,
                     "Explicit half-box clip; opaque interior and exact crosshair")},
                // Donner-rendered golden; see donner/svg/renderer/testdata/golden/README.md.
                {"in=BackgroundAlpha.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/donner-in-BackgroundAlpha.png",
                     kDefaultThreshold,
                     "Reference disagreement: the PNG blurs with a different valid Gaussian "
                     "approximation than its in=BackgroundImage sibling, so the two references "
                     "cannot both match one conforming renderer; Donner uses the specified "
                     "three-box approximation and keeps BackgroundAlpha equal to BackgroundImage's "
                     "alpha")},
            })),
        ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFilterFunctions, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "filters/filter-functions",
                {
                    // Donner-rendered goldens; see donner/svg/renderer/testdata/golden/README.md.
                    {"one-invalid-url-in-list.svg",
                     Params::WithGoldenOverride(
                         "donner/svg/renderer/testdata/golden/donner-one-invalid-url-in-list.png",
                         kDefaultThreshold,
                         "Reference disagreement: the PNG applies the functions around a missing "
                         "url(); a missing reference disables the whole filter chain "
                         "(FilterReferenceStages.InvalidReferenceDisablesEntireChain)")},
                    {"color-adjust-functions-0percent.svg",
                     ColorAdjustReference("donner/svg/renderer/testdata/golden/"
                                          "donner-color-adjust-functions-0percent.png",
                                          "donner/svg/renderer/testdata/golden/geode/"
                                          "donner-color-adjust-functions-0percent.png")},
                    {"color-adjust-functions-100percent.svg",
                     ColorAdjustReference("donner/svg/renderer/testdata/golden/"
                                          "donner-color-adjust-functions-100percent.png",
                                          "donner/svg/renderer/testdata/golden/geode/"
                                          "donner-color-adjust-functions-100percent.png")},
                    {"color-adjust-functions-2.svg",
                     ColorAdjustReference("donner/svg/renderer/testdata/golden/"
                                          "donner-color-adjust-functions-200percent.png",
                                          "donner/svg/renderer/testdata/golden/geode/"
                                          "donner-color-adjust-functions-200percent.png")},
                    {"color-adjust-functions-200percent.svg",
                     ColorAdjustReference("donner/svg/renderer/testdata/golden/"
                                          "donner-color-adjust-functions-200percent.png",
                                          "donner/svg/renderer/testdata/golden/geode/"
                                          "donner-color-adjust-functions-200percent.png")},
                    {"color-adjust-functions-50percent.svg",
                     ColorAdjustReference("donner/svg/renderer/testdata/golden/"
                                          "donner-color-adjust-functions-50percent.png",
                                          "donner/svg/renderer/testdata/golden/geode/"
                                          "donner-color-adjust-functions-50percent.png")},
                    {"color-adjust-functions-default-value.svg",
                     ColorAdjustReference("donner/svg/renderer/testdata/golden/"
                                          "donner-color-adjust-functions-100percent.png",
                                          "donner/svg/renderer/testdata/golden/geode/"
                                          "donner-color-adjust-functions-100percent.png")},
                    {"color-adjust-functions-negative.svg",
                     ColorAdjustReference("donner/svg/renderer/testdata/golden/"
                                          "donner-color-adjust-functions-negative.png",
                                          "donner/svg/renderer/testdata/golden/geode/"
                                          "donner-color-adjust-functions-negative.png")},
                    // Donner-rendered goldens; see donner/svg/renderer/testdata/golden/README.md.
                    {"drop-shadow-function-color-last.svg",
                     DropShadowFunctionGolden(
                         "donner/svg/renderer/testdata/golden/donner-drop-shadow-function.png",
                         "Reference disagreement: resvg truncates the fractional shadow offset")},
                    {"drop-shadow-function-em-values.svg",
                     DropShadowFunctionGolden("donner/svg/renderer/testdata/golden/"
                                              "donner-drop-shadow-function-em-values.png",
                                              "Reference disagreement: the reference places the "
                                              "shadow at truncated lengths")},
                    {"drop-shadow-function-mm-values.svg",
                     DropShadowFunctionGolden("donner/svg/renderer/testdata/golden/"
                                              "donner-drop-shadow-function-mm-values.png",
                                              "Reference disagreement: the reference places the "
                                              "shadow at truncated lengths")},
                    {"drop-shadow-function.svg",
                     DropShadowFunctionGolden(
                         "donner/svg/renderer/testdata/golden/donner-drop-shadow-function.png",
                         "Reference disagreement: resvg truncates the fractional shadow offset")},
                    {"two-exact-urls.svg",
                     Params::UndefinedBehavior(
                         "reference is the resvg UB marker image; the valid "
                         "repeated filter URL has an independent equivalent-graph "
                         "comparison (#1169)")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(FiltersFloodColor, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("filters/flood-color")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(FiltersFloodOpacity, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("filters/flood-opacity")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    MaskingClip, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "masking/clip",
                {
                    {"simple-case.svg",
                     Params::DeprecatedInSVG2("CSS `clip` property (rect() clipping, deprecated "
                                              "by CSS Masking in favor of clip-path)")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(MaskingClipRule, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("masking/clip-rule")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    MaskingClipPath, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "masking/clipPath",
                {
                    // The upstream resvg reference maps the clip through the text's glyph cells;
                    // the snapshot's maps it through the glyph ink extents.
                    {"clip-path-with-transform-on-text.svg",
                     Params().requireFeature(RendererBackendFeature::Text, "text rendering")},
                    {"on-the-root-svg-without-size.svg",
                     Params::UndefinedBehavior("on root `<svg>` without size")},

                    // Donner-rendered goldens; see donner/svg/renderer/testdata/golden/README.md.
                    {"circle-shorthand-with-stroke-box.svg",
                     Params::WithGoldenOverride(
                         "donner/svg/renderer/testdata/golden/"
                         "donner-circle-shorthand-with-stroke-box.png",
                         kDefaultThreshold,
                         "Harness mismatch: the PNG is opaque white where the canvas is "
                         "transparent (CircleClipPath.DefaultAndStrokeBoxIncludeStrokeGeometry)")},
                    {"circle-shorthand-with-view-box.svg",
                     Params::WithGoldenOverride(
                         "donner/svg/renderer/testdata/golden/"
                         "donner-circle-shorthand-with-view-box.png",
                         kDefaultThreshold,
                         "Harness mismatch: the PNG is opaque white where the canvas is "
                         "transparent (CircleClipPath.ViewBoxUsesNearestSvgViewport)")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    MaskingMask, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory("masking/mask",
                                        {
                                            {"recursive-on-child.svg",
                                             Params::UndefinedBehavior("Recursive on child")},
                                        })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintServersLinearGradient, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory(
                                     "paint-servers/linearGradient",
                                     {
                                         {"invalid-gradientTransform.svg",
                                          Params::UndefinedBehavior("Invalid `gradientTransform`")},
                                     })),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintServersPattern, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "paint-servers/pattern",
                {
                    {"invalid-patternTransform.svg",
                     Params::UndefinedBehavior("Invalid patternTransform")},
                    {"out-of-order-referencing.svg",
                     Params::WithGoldenOverride(
                         "donner/svg/renderer/testdata/golden/resvg-nested-rectangles.png",
                         kDefaultThreshold,
                         "Reference disagreement: against the authored nested tile geometry, 832 "
                         "pixels are opaque in one image and empty in the other; resvg 0.49.0 "
                         "renders the tiles drawn explicitly")},
                    {"overflow=visible.svg", Params::UndefinedBehavior("overflow=visible")},
                    // The references resample a rasterized tile, blurring the tile-edge strokes.
                    // Donner also rasterizes each tile, at 2x device scale, and samples it
                    // bilinearly; at these tiles' alignment that keeps the seams sharp.
                    // testdata/golden/README.md has the measured seam coverage.
                    {"recursive-on-child.svg",
                     Params::WithGoldenOverride("donner/svg/renderer/testdata/golden/"
                                                "donner-pattern-recursive-on-child.png")
                         .withReason("Reference disagreement: the vendored PNG blurs the tile-edge "
                                     "strokes that Donner's 2x tile raster keeps sharp at this "
                                     "alignment, and the mutually recursive fill paints one level "
                                     "deep (#1185)")},
                    {"self-recursive-on-child.svg",
                     Params::WithGoldenOverride("donner/svg/renderer/testdata/golden/"
                                                "donner-pattern-self-recursive.png")
                         .withReason("Reference disagreement: the vendored PNG blurs the tile-edge "
                                     "strokes that Donner's 2x tile raster keeps sharp at this "
                                     "alignment (#1185)")},
                    {"self-recursive.svg",
                     Params::WithGoldenOverride("donner/svg/renderer/testdata/golden/"
                                                "donner-pattern-self-recursive.png")
                         .withReason("Reference disagreement: the vendored PNG blurs the tile-edge "
                                     "strokes that Donner's 2x tile raster keeps sharp at this "
                                     "alignment (#1185)")},
                    {"text-child.svg",
                     Params::WithGoldenOverride(
                         "donner/svg/renderer/testdata/golden/resvg-skewed-text-tiles.png",
                         kDefaultThreshold,
                         "Reference disagreement: the vendored PNG resamples a rasterized skewed "
                         "tile, spreading text and border edges into empty cells; resvg 0.49.0 "
                         "renders the clipped tiles drawn explicitly")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintServersRadialGradient, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "paint-servers/radialGradient",
            {
                {"fr=-1.svg",
                 Params::UndefinedBehavior("reference is the resvg UB marker image; "
                                           "invalid negative focal radius: no paint (#1186)")},
                {"fr=0.5.svg",
                 Params::UndefinedBehavior("reference is the resvg UB marker image; coincident "
                                           "start/end circles paint nothing today, an open "
                                           "question in svgwg issue 648 (#1186)")},
                {"invalid-gradientTransform.svg",
                 Params::UndefinedBehavior(
                     "reference is the resvg UB marker image; singular transform: no paint "
                     "(#1186)")},
                {"invalid-gradientUnits.svg",
                 Params::UndefinedBehavior("reference is the resvg UB marker image; invalid units "
                                           "retain the objectBoundingBox default (#1186)")},
                {"negative-r.svg",
                 Params::UndefinedBehavior(
                     "reference is the resvg UB marker image; invalid negative radius: no "
                     "paint (#1186)")},
            })),
        ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintServersStop, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("paint-servers/stop")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintServersStopColor, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("paint-servers/stop-color")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintServersStopOpacity, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("paint-servers/stop-opacity")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintingColor, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("painting/color")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintingContext, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("painting/context")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintingDisplay, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory("painting/display",
                                        {
                                            {"none-on-tref.svg",
                                             Params::DeprecatedInSVG2("<tref> (removed in SVG 2)")},
                                        })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintingFill, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "painting/fill",
                {
                    {"icc-color.svg",
                     Params::DeprecatedInSVG2("SVG 1.1 icc-color() paint (removed in SVG 2); the "
                                              "reference is the resvg UB marker image")},
                    {"rgb-int-int-int.svg",
                     Params::UndefinedBehavior("reference is the resvg UB marker image; valid CSS "
                                               "Color 4 space-separated RGB (#1186)")},
                    {"valid-FuncIRI-with-a-fallback-ICC-color.svg",
                     Params::DeprecatedInSVG2("SVG 1.1 icc-color() paint fallback (removed in "
                                              "SVG 2); the invalid declaration is rejected "
                                              "(#1186)")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintingFillOpacity, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("painting/fill-opacity")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintingFillRule, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("painting/fill-rule")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintingImageRendering, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "painting/image-rendering",
            {
                {"on-feImage.svg",
                 Params().requireFeature(RendererBackendFeature::FilterEffects, "filter effects")},
            })),
        ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintingIsolation, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("painting/isolation")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintingMarker, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "painting/marker",
            {
                {"target-with-subpaths-2.svg", Params::UndefinedBehavior("Target with subpaths")},
                {"with-a-text-child.svg", GeodeUnresolvedEdgeCoverageGate(Params().requireFeature(
                                              RendererBackendFeature::Text, "text rendering"))},
                // Geode-rendered golden; see donner/svg/renderer/testdata/golden/README.md.
                {"with-an-image-child.svg",
                 Params().withGeodeGoldenOverride(
                     "donner/svg/renderer/testdata/golden/geode/"
                     "painting_marker_with-an-image-child.png",
                     "Accepted rasterization difference: the reference draws the 0.40 device px "
                     "letter strokes as 1 px hairlines with alpha scaled by the width, and Geode "
                     "covers their exact geometry; total dark ink agrees within 0.2%")},
                {"with-viewBox-1.svg", Params::UndefinedBehavior("with `viewBox`")},
            })),
        ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintingMixBlendMode, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("painting/mix-blend-mode")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintingOpacity, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("painting/opacity")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintingOverflow, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("painting/overflow")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintingPaintOrder, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("painting/paint-order")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintingShapeRendering, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("painting/shape-rendering")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintingStroke, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "painting/stroke",
                {
                    // SVG uses glyph cells for a text element's object bounding box. The
                    // vendored images use ink extents; the Firefox references use glyph cells.
                    {"linear-gradient-on-text.svg",
                     GradientStrokeOnTextGoldens("donner/svg/renderer/testdata/golden/"
                                                 "firefox-stroke-linear-gradient-on-text.png",
                                                 "donner/svg/renderer/testdata/golden/geode/"
                                                 "painting_stroke_linear-gradient-on-text.png")},
                    {"radial-gradient-on-text.svg",
                     GradientStrokeOnTextGoldens("donner/svg/renderer/testdata/golden/"
                                                 "firefox-stroke-radial-gradient-on-text.png",
                                                 "donner/svg/renderer/testdata/golden/geode/"
                                                 "painting_stroke_radial-gradient-on-text.png")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintingStrokeDasharray, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "painting/stroke-dasharray",
                {
                    {"n-0.svg", Params::WithThreshold(0.0f, 0, "Closed-path dash seam identity")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintingStrokeDashoffset, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("painting/stroke-dashoffset")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintingStrokeLinecap, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("painting/stroke-linecap")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintingStrokeLinejoin, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "painting/stroke-linejoin",
                {
                    {"arcs.svg", Params::UndefinedBehavior(
                                     "reference is the resvg UB marker image; SVG "
                                     "Strokes draft arcs is not implemented, documented miter "
                                     "fallback (#1186)")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintingStrokeMiterlimit, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("painting/stroke-miterlimit")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintingStrokeOpacity, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("painting/stroke-opacity")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintingStrokeWidth, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "painting/stroke-width",
                {
                    {"negative.svg", Params::UndefinedBehavior("Nothing should be rendered")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintingVisibility, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "painting/visibility",
                {
                    // The upstream resvg reference maps the clip through the two text elements'
                    // glyph cells; the snapshot's maps it through the union of their glyph ink
                    // extents (measured 50.5..144.4 x 69.3..105.5 user units).
                    {"bbox-impact-3.svg",
                     Params().requireFeature(RendererBackendFeature::Text, "text rendering")},
                    {"collapse-on-tspan.svg",
                     Params()
                         .requireFeature(RendererBackendFeature::Text, "text rendering")
                         .withGeodeQuarterPixelEdgeCoverage()},
                    {"hidden-on-tspan.svg",
                     Params()
                         .requireFeature(RendererBackendFeature::Text, "text rendering")
                         .withGeodeQuarterPixelEdgeCoverage()},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(ShapesCircle, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("shapes/circle")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(ShapesEllipse, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("shapes/ellipse")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(ShapesLine, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("shapes/line")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(ShapesPath, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("shapes/path")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(ShapesPolygon, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("shapes/polygon")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(ShapesPolyline, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("shapes/polyline")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(ShapesRect, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("shapes/rect")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(StructureA, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("structure/a")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(StructureDefs, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("structure/defs")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(StructureG, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("structure/g")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StructureImage, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "structure/image",
                {
                    {"float-size.svg", Params::UndefinedBehavior("Float size")},
                    {"no-height-on-svg.svg", Params::UndefinedBehavior("No height")},
                    {"no-width-and-height-on-svg.svg",
                     Params::UndefinedBehavior("No width and height")},
                    {"no-width-on-svg.svg", Params::UndefinedBehavior("No width")},
                    // Harness mismatch: the harness gives SVG image documents the suite's test
                    // fonts as a stand-in for system fonts, so the row compares against a Donner
                    // rendering of that configuration. The vendored reference omits the embedded
                    // text and the resvg v0.49.0 one quantizes the frame lines' coverage to
                    // quarters; see testdata/golden/README.md.
                    {"embedded-svg-with-text.svg",
                     Params::WithGoldenOverride(
                         "donner/svg/renderer/testdata/golden/resvg-embedded-svg-with-text.png")
                         .withReason("Harness mismatch: SVG images get the suite's test fonts")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StructureStyle, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "structure/style",
                {
                    {"non-presentational-attribute.svg",
                     Params::DeprecatedInSVG2("<svg version=\"1.1\"> semantics (the version "
                                              "attribute is removed in SVG 2)")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StructureStyleAttribute, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "structure/style-attribute",
                {
                    {"non-presentational-attribute.svg",
                     Params::DeprecatedInSVG2("<svg version=\"1.1\"> disables geometry "
                                              "attributes in style (the version attribute is "
                                              "removed in SVG 2)")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StructureSvg, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "structure/svg",
            {
                {"funcIRI-parsing.svg", Params::UndefinedBehavior("FuncIRI parsing")},
                {"funcIRI-with-invalid-characters.svg",
                 Params::UndefinedBehavior(
                     "The upstream reference is the UB marker for a digit-leading XML ID")},
                {"invalid-id-attribute-1.svg", Params::UndefinedBehavior("Invalid id attribute")},
                {"invalid-id-attribute-2.svg", Params::UndefinedBehavior("Invalid id attribute")},
                // Donner-rendered golden; see donner/svg/renderer/testdata/golden/README.md.
                {"no-size.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/donner-no-size.png", kDefaultThreshold,
                     "API policy: an unsized root gets the caller's canvas at one user unit per "
                     "px; the PNG shows a 200-unit viewport at 2.5x "
                     "(ReferenceContractUnsizedRootUsesCallerCanvasWithoutContentFit)")},
                {"not-UTF-8-encoding.svg",
                 Params::Skip("Unsupported input: XML byte parser requires UTF-8; transcode "
                              "and update the encoding declaration (#1182)")},

                {"funcIRI-with-quotes.svg", Params{}},
                {"nested-svg-one-with-rect-and-one-with-viewBox.svg", Params{}},
            })),
        ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(StructureSwitch, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("structure/switch")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(StructureSymbol, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("structure/symbol")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(StructureSystemLanguage, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("structure/systemLanguage")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(StructureTransform, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("structure/transform")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(StructureTransformOrigin, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("structure/transform-origin")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StructureUse, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "structure/use",
                {
                    // Donner-rendered golden; see donner/svg/renderer/testdata/golden/README.md.
                    {"xlink-to-an-external-file.svg",
                     Params::WithGoldenOverride(
                         "donner/svg/renderer/testdata/golden/donner-xlink-to-an-external-file.png",
                         kDefaultThreshold,
                         "Harness mismatch: the PNG is opaque white where the canvas is "
                         "transparent; reference glyph rasterization: its glyph ink is 5.6% "
                         "heavier (ExternalUseTextFragmentUsesHostStyleAndTransparentCanvas)")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextAlignmentBaseline, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/alignment-baseline",
                {
                    {"alphabetic.svg", QuarterPixelEdgeReference()},
                    {"auto.svg", QuarterPixelEdgeReference()},
                    {"before-edge.svg", QuarterPixelEdgeReference()},
                    {"central.svg", QuarterPixelEdgeReference()},
                    {"hanging-and-baseline-shift-eq-20-on-tspan.svg", QuarterPixelEdgeReference()},
                    {"hanging-on-tspan.svg", QuarterPixelEdgeReference()},
                    {"hanging-with-underline.svg", QuarterPixelEdgeReference()},
                    {"hanging.svg", QuarterPixelEdgeReference()},
                    {"inherit.svg", QuarterPixelEdgeReference()},
                    {"mathematical.svg", QuarterPixelEdgeReference()},
                    {"middle.svg", QuarterPixelEdgeReference()},
                    {"text-before-edge.svg", QuarterPixelEdgeReference()},
                    // UB: resvg's golden PNG is a "UB" placeholder overlay, so no render can
                    // match it. Render for no-crash coverage only.
                    {"after-edge.svg",
                     Params::UndefinedBehavior("reference is the resvg UB marker image")},
                    {"baseline.svg",
                     Params::UndefinedBehavior("reference is the resvg UB marker image")},
                    {"ideographic.svg",
                     Params::UndefinedBehavior("reference is the resvg UB marker image")},
                    {"text-after-edge.svg",
                     Params::UndefinedBehavior("reference is the resvg UB marker image")},

                    {"hanging-on-vertical.svg",
                     Params::WithGoldenOverride(
                         "donner/svg/renderer/testdata/golden/resvg-hanging-on-vertical.png")
                         .withReason("Reference disagreement: the PNG advances upright glyphs "
                                     "1.39 em, not the 1 em vmtx advance CSS Writing Modes 3 "
                                     "5.1.1 requires (golden README)")
                         .withGeodeQuarterPixelEdgeCoverage()},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextBaselineShift, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory("text/baseline-shift",
                                        {{"-10.svg", QuarterPixelEdgeReference()},
                                         {"-50percent.svg", QuarterPixelEdgeReference()},
                                         {"0.svg", QuarterPixelEdgeReference()},
                                         {"10.svg", QuarterPixelEdgeReference()},
                                         {"2mm.svg", QuarterPixelEdgeReference()},
                                         {"50percent.svg", QuarterPixelEdgeReference()},
                                         {"baseline.svg", QuarterPixelEdgeReference()},
                                         {"deeply-nested-super.svg", QuarterPixelEdgeReference()},
                                         {"inheritance-1.svg", QuarterPixelEdgeReference()},
                                         {"inheritance-2.svg", QuarterPixelEdgeReference()},
                                         {"inheritance-3.svg", QuarterPixelEdgeReference()},
                                         {"inheritance-4.svg", QuarterPixelEdgeReference()},
                                         {"inheritance-5.svg", QuarterPixelEdgeReference()},
                                         {"invalid-value.svg", QuarterPixelEdgeReference()},
                                         {"mixed-nested.svg", QuarterPixelEdgeReference()},
                                         {"nested-length.svg", QuarterPixelEdgeReference()},
                                         {"nested-super.svg", QuarterPixelEdgeReference()},
                                         {"sub.svg", QuarterPixelEdgeReference()},
                                         {"super.svg", QuarterPixelEdgeReference()},
                                         {"with-rotate.svg", QuarterPixelEdgeReference()}})),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextDirection, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/direction",
                {
                    {"rtl-with-vertical-writing-mode.svg",
                     Params::UndefinedBehavior("rtl with vertical writing-mode (svgwg#618)")},
                    {"rtl.svg",
                     Params().onlyTextFull().withReason("Arabic shaping needs HarfBuzz")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextDominantBaseline, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/dominant-baseline",
                {
                    {"alignment-baseline-and-baseline-shift-on-tspans.svg",
                     QuarterPixelEdgeReference()},
                    {"alignment-baseline=baseline-on-tspan.svg", QuarterPixelEdgeReference()},
                    {"alphabetic.svg", QuarterPixelEdgeReference()},
                    {"auto.svg", QuarterPixelEdgeReference()},
                    {"central.svg", QuarterPixelEdgeReference()},
                    {"complex.svg", QuarterPixelEdgeReference()},
                    {"different-alignment-baseline-on-tspan.svg", QuarterPixelEdgeReference()},
                    {"dummy-tspan.svg", QuarterPixelEdgeReference()},
                    {"equal-alignment-baseline-on-tspan.svg", QuarterPixelEdgeReference()},
                    {"ideographic.svg", QuarterPixelEdgeReference()},
                    {"inherit.svg", QuarterPixelEdgeReference()},
                    {"mathematical.svg", QuarterPixelEdgeReference()},
                    {"middle.svg", QuarterPixelEdgeReference()},
                    {"nested.svg", QuarterPixelEdgeReference()},
                    {"no-change.svg", QuarterPixelEdgeReference()},
                    {"sequential.svg", QuarterPixelEdgeReference()},
                    {"text-after-edge.svg", QuarterPixelEdgeReference()},
                    {"text-before-edge.svg", QuarterPixelEdgeReference()},
                    // UB: resvg's golden PNG is a "UB" placeholder overlay, so no render can
                    // match it. Render for no-crash coverage only.
                    {"reset-size.svg",
                     Params::DeprecatedInSVG2("dominant-baseline: reset-size (SVG 1.1 value, "
                                              "removed in SVG 2); the reference is the resvg UB "
                                              "marker image")},
                    {"use-script.svg",
                     Params::DeprecatedInSVG2("dominant-baseline: use-script (SVG 1.1 value, "
                                              "removed in SVG 2); the reference is the resvg UB "
                                              "marker image")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextFont, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "text/font",
            {
                // SVG 2 has no `font` presentation attribute, so the case draws the initial
                // font. The vendored 400x400 PNG applies the attribute as SVG 1.1 did and
                // synthesizes bold over Noto Sans Italic. Donner-rendered golden; see
                // donner/svg/renderer/testdata/golden/README.md.
                {"simple-case.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/donner-font-simple-case.png",
                     kDefaultThreshold,
                     "Reference disagreement: the PNG applies the `font` attribute, which SVG 2 "
                     "does not list as a presentation attribute "
                     "(ReferenceContractFontAttributeIsIgnored)")},
            })),
        ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

// The generic-family references are the upstream resvg renderings (provenance in
// third_party/resvg-test-suite/BUILD.bazel). Which face each family resolves to is asserted
// directly in FontSelection_tests.cc; an all-unknown family list resolves to the harness's default
// family, serif, as in resvg's harness
// (FontSelection.UnmatchedFamilyListUsesTheSuiteDefaultFamily).
INSTANTIATE_TEST_SUITE_P(TextFontFamily, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("text/font-family")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextFontKerning, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/font-kerning",
                {{"as-property.svg", QuarterPixelEdgeReference()},
                 {"none.svg", QuarterPixelEdgeReference()},
                 {"arabic-script.svg",
                  Params().onlyTextFull().withReason("Arabic contextual joining requires HarfBuzz; "
                                                     "disabling kerning must preserve joining")}})),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextFontSize, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "text/font-size",
            {
                // The PNGs scale named sizes by the CSS 2 factor of 1.2 per step; Donner follows
                // the CSS Fonts 4 absolute-size table. testdata/golden/README.md has the evidence.
                {"named-value-without-a-parent.svg",
                 Params::WithGoldenOverride("donner/svg/renderer/testdata/golden/"
                                            "donner-named-value-without-a-parent.png")
                     .withReason("Reference disagreement: Donner follows the CSS Fonts 4 "
                                 "absolute-size table; the PNG steps by 1.2 as CSS 2 did")},
                {"named-value.svg",
                 GeodeUnquantizedGoldenGate(
                     Params::WithGoldenOverride(
                         "donner/svg/renderer/testdata/golden/donner-named-value.png")
                         .withReason("Reference disagreement: Donner follows the CSS Fonts 4 "
                                     "absolute-size table; the PNG steps by 1.2 as CSS 2 did"))},
                {"negative-size.svg", Params::UndefinedBehavior("negative font size")},
            })),
        ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextFontSizeAdjust, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/font-size-adjust",
                // The PNG centers the kerned text on its unkerned advance (4.03 device px left), so
                // the golden is Donner's rendering; testdata/golden/README.md has the evidence.
                {{"simple-case.svg",
                  Params::WithGoldenOverride(
                      "donner/svg/renderer/testdata/golden/resvg-font-size-adjust-simple-case.png")
                      .withReason("Reference disagreement: text-anchor centers the shaped "
                                  "(kerned) advance (#1173)")}})),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(TextFontStretch, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("text/font-stretch")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(TextFontStyle, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("text/font-style")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextFontVariant, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/font-variant",
                {
                    {"inherit.svg",
                     GeodeCpuAllowanceGate(Params().withSimpleTextMaxPixels(1200).withReason(
                         "small-caps is emulated with simple text"))},
                    {"small-caps.svg",
                     GeodeCpuAllowanceGate(Params().withSimpleTextMaxPixels(1200).withReason(
                         "small-caps is emulated with simple text"))},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(TextFontWeight, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("text/font-weight")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextGlyphOrientationHorizontal, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/glyph-orientation-horizontal",
                {
                    {"simple-case.svg",
                     Params::DeprecatedInSVG2("glyph-orientation-horizontal (removed in SVG 2)")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextGlyphOrientationVertical, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/glyph-orientation-vertical",
                {
                    // SVG 2 aliases glyph-orientation-vertical="0" to upright text, which
                    // RendererRegressionTests.GlyphOrientationVerticalZeroDrawsUprightGlyphs
                    // checks. Noto Sans has no vertical metrics, and CSS Writing Modes 3 leaves
                    // their synthesis to the user agent, so the glyph positions are not defined.
                    {"simple-case.svg",
                     Params::UndefinedBehavior(
                         "upright glyphs of a face without vertical metrics; CSS Writing Modes 3 "
                         "does not define how the user agent synthesizes them")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextKerning, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/kerning",
                {
                    {"0.svg", Params::DeprecatedInSVG2("kerning property (removed in SVG 2)")},
                    {"10percent.svg",
                     Params::DeprecatedInSVG2("kerning property (removed in SVG 2)")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextLengthAdjust, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory("text/lengthAdjust",
                                        {{"spacingAndGlyphs.svg", QuarterPixelEdgeReference()}})),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextLetterSpacing, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/letter-spacing",
                {
                    {"-3.svg", QuarterPixelEdgeReference()},
                    {"0.svg", QuarterPixelEdgeReference()},
                    {"1mm.svg", QuarterPixelEdgeReference()},
                    {"3.svg", QuarterPixelEdgeReference()},
                    {"5percent.svg", QuarterPixelEdgeReference()},
                    {"mixed-spacing.svg", QuarterPixelEdgeReference()},
                    {"normal.svg", QuarterPixelEdgeReference()},
                    {"large-negative.svg", Params::UndefinedBehavior("negative letter-spacing")},
                    {"mixed-scripts.svg", Params().onlyTextFull().withReason("Arabic shaping")},
                    {"on-Arabic.svg", Params()
                                          .requireFeature(RendererBackendFeature::TextFull)
                                          .withReason("Arabic text")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextText, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/text",
                {
                    {"dx-and-dy-instead-of-x-and-y.svg", QuarterPixelEdgeReference()},
                    {"dx-and-dy-with-less-values-than-characters.svg", QuarterPixelEdgeReference()},
                    {"dx-and-dy-with-more-values-than-characters.svg", QuarterPixelEdgeReference()},
                    {"dx-and-dy-with-multiple-values.svg", QuarterPixelEdgeReference()},
                    {"em-and-ex-coordinates.svg", QuarterPixelEdgeReference()},
                    {"escaped-text-1.svg", QuarterPixelEdgeReference()},
                    {"escaped-text-2.svg", QuarterPixelEdgeReference()},
                    {"escaped-text-3.svg", QuarterPixelEdgeReference()},
                    {"escaped-text-4.svg", QuarterPixelEdgeReference()},
                    {"mm-coordinates.svg", QuarterPixelEdgeReference()},
                    {"nested.svg", QuarterPixelEdgeReference()},
                    {"no-coordinates.svg", QuarterPixelEdgeReference()},
                    {"percent-value-on-dx-and-dy.svg", QuarterPixelEdgeReference()},
                    {"percent-value-on-x-and-y.svg", QuarterPixelEdgeReference()},
                    {"rotate-with-an-invalid-angle.svg", QuarterPixelEdgeReference()},
                    {"rotate-with-less-values-than-characters.svg", QuarterPixelEdgeReference()},
                    {"rotate-with-more-values-than-characters.svg", QuarterPixelEdgeReference()},
                    {"rotate-with-multiple-values-underline-and-pattern.svg",
                     QuarterPixelEdgeReference()},
                    {"rotate-with-multiple-values.svg", QuarterPixelEdgeReference()},
                    {"rotate.svg", QuarterPixelEdgeReference()},
                    {"simple-case.svg", QuarterPixelEdgeReference()},
                    {"transform.svg", QuarterPixelEdgeReference()},
                    {"x-and-y-with-dx-and-dy-lists.svg", QuarterPixelEdgeReference()},
                    {"x-and-y-with-dx-and-dy.svg", QuarterPixelEdgeReference()},
                    {"x-and-y-with-less-values-than-characters.svg", QuarterPixelEdgeReference()},
                    {"x-and-y-with-more-values-than-characters.svg", QuarterPixelEdgeReference()},
                    {"x-and-y-with-multiple-values-and-tspan.svg", QuarterPixelEdgeReference()},
                    {"x-and-y-with-multiple-values.svg", QuarterPixelEdgeReference()},
                    {"xml-space.svg", QuarterPixelEdgeReference()},
                    {"bidi-reordering.svg", Params().onlyTextFull().withReason("Arabic shaping")},
                    {"complex-grapheme-split-by-tspan.svg",
                     Params::UndefinedBehavior("grapheme split by tspan")},
                    {"complex-graphemes-and-coordinates-list.svg",
                     Params::WithGoldenOverride("donner/svg/renderer/testdata/golden/"
                                                "resvg-complex-graphemes-and-coordinates-list.png")
                         .onlyTextFull()
                         .withReason("The composed Cyrillic cluster uses its first absolute y")},
                    {"complex-graphemes.svg",
                     Params().onlyTextFull().withReason("Combining mark needs HarfBuzz")},
                    {"compound-emojis-and-coordinates-list.svg",
                     EmojiGuideGolden("donner/svg/renderer/testdata/golden/"
                                      "donner-compound-emojis-and-coordinates-list.png")},
                    {"compound-emojis.svg",
                     EmojiGuideGolden(
                         "donner/svg/renderer/testdata/golden/donner-compound-emojis.png")},
                    {"emojis.svg",
                     EmojiGuideGolden("donner/svg/renderer/testdata/golden/donner-emojis.png")},
                    {"fill-rule=evenodd.svg",
                     Params().onlyTextFull().withReason("Arabic text shaping requires text-full")},
                    {"rotate-on-Arabic.svg",
                     Params().onlyTextFull().withReason("Arabic text shaping requires text-full")},
                    {"rotate-with-multiple-values-and-complex-text.svg",
                     Params().onlyTextFull().withReason("Complex diatrics requires text-full")},
                    {"x-and-y-with-multiple-values-and-arabic-text.svg",
                     Params().onlyTextFull().withReason("Arabic text shaping")},
                    {"zalgo.svg",
                     Params().onlyTextFull().withReason("Stacked combining marks need HarfBuzz")},
                    // Only the full tier applies the Amiri font's GSUB fi ligature. The SVGs
                    // swap the order of the Amiri and Noto Sans runs to test both boundaries.
                    {"ligatures-handling-in-mixed-fonts-1.svg",
                     Params().onlyTextFull().withReason(
                         "Mixed-font ligature shaping requires HarfBuzz")},
                    {"ligatures-handling-in-mixed-fonts-2.svg",
                     Params().onlyTextFull().withReason(
                         "Mixed-font ligature shaping requires HarfBuzz")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextTextAnchor, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/text-anchor",
                {
                    {"end-on-text.svg", QuarterPixelEdgeReference()},
                    {"end-with-letter-spacing.svg", QuarterPixelEdgeReference()},
                    {"inheritance-1.svg", QuarterPixelEdgeReference()},
                    {"inheritance-2.svg", QuarterPixelEdgeReference()},
                    {"inheritance-3.svg", QuarterPixelEdgeReference()},
                    {"invalid-value-on-text.svg", QuarterPixelEdgeReference()},
                    {"middle-on-text.svg", QuarterPixelEdgeReference()},
                    {"on-the-first-tspan.svg", QuarterPixelEdgeReference()},
                    {"on-tspan.svg", QuarterPixelEdgeReference()},
                    {"start-on-text.svg", QuarterPixelEdgeReference()},
                    {"text-anchor-not-on-text-chunk.svg", QuarterPixelEdgeReference()},
                    // The vendored PNG is opaque and resvg v0.49.0 anchors the y-only chunk
                    // differently; testdata/golden/README.md has the evidence.
                    {"coordinates-list.svg",
                     Params::WithGoldenOverride(
                         "donner/svg/renderer/testdata/golden/donner-coordinates-list.png")
                         .withReason("Harness mismatch: opaque reference background; under 4x4 "
                                     "supersampled coverage the glyphs match it (#1185)")
                         .withGeodeQuarterPixelEdgeCoverage()},
                    {"on-tspan-with-arabic.svg",
                     Params()
                         .requireFeature(RendererBackendFeature::TextFull)
                         .withReason("Arabic contextual joining requires HarfBuzz")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextTextDecoration, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/text-decoration",
                {
                    {"underline-with-dy-list-2.svg", GeodeUnresolvedEdgeCoverageGate(Params())},
                    {"all-types-nested.svg", QuarterPixelEdgeReference()},
                    {"line-through.svg", QuarterPixelEdgeReference()},
                    {"outside-the-text-element.svg", QuarterPixelEdgeReference()},
                    {"overline.svg", QuarterPixelEdgeReference()},
                    {"style-resolving-1.svg", QuarterPixelEdgeReference()},
                    {"style-resolving-2.svg", QuarterPixelEdgeReference()},
                    {"style-resolving-3.svg", QuarterPixelEdgeReference()},
                    {"style-resolving-4.svg", QuarterPixelEdgeReference()},
                    {"underline-with-dy-list-1.svg", QuarterPixelEdgeReference()},
                    {"underline-with-rotate-list-3.svg", QuarterPixelEdgeReference()},
                    {"underline-with-y-list.svg", QuarterPixelEdgeReference()},
                    {"underline.svg", QuarterPixelEdgeReference()},
                    {"all-types-inline.svg",
                     Params::WithGoldenOverride("donner/svg/renderer/testdata/golden/"
                                                "resvg-all-types-inline-bands.png")
                         .withReason("Literal bands preserve multiline decoration "
                                     "offsets and paint order")},
                    {"tspan-decoration.svg",
                     GeodeUnresolvedEdgeCoverageGate(Params::WithThreshold(
                         0.09f, kDefaultMismatchedPixels,
                         "Archived nested underline and glyph coverage differ"))},
                    {"underline-with-rotate-list-4.svg",
                     Params::WithThreshold(0.03f, kDefaultMismatchedPixels,
                                           "Reference has a wider horizontal fill "
                                           "gradient across rotated glyphs")
                         .withGeodeQuarterPixelEdgeCoverage()},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextTextRendering, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory("text/text-rendering",
                                        {{"geometricPrecision.svg", QuarterPixelEdgeReference()},
                                         {"on-tspan.svg", QuarterPixelEdgeReference()},
                                         {"optimizeLegibility.svg", QuarterPixelEdgeReference()},
                                         {"optimizeSpeed.svg", QuarterPixelEdgeReference()},
                                         {"with-underline.svg", QuarterPixelEdgeReference()}})),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextTextLength, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/textLength",
                {
                    {"150-on-parent.svg", QuarterPixelEdgeReference()},
                    {"150-on-tspan.svg", QuarterPixelEdgeReference()},
                    {"150.svg", QuarterPixelEdgeReference()},
                    {"40mm.svg", QuarterPixelEdgeReference()},
                    {"75percent.svg", QuarterPixelEdgeReference()},
                    {"inherit.svg", QuarterPixelEdgeReference()},
                    {"negative.svg", QuarterPixelEdgeReference()},
                    {"on-a-single-tspan.svg", QuarterPixelEdgeReference()},
                    {"on-text-and-tspan.svg", QuarterPixelEdgeReference()},
                    {"arabic-with-lengthAdjust.svg",
                     Params().onlyTextFull().withReason("Arabic shaping needs HarfBuzz")},
                    {"arabic.svg",
                     Params().onlyTextFull().withReason("Arabic shaping needs HarfBuzz")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextTextPath, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "text/textPath",
            {
                // Both draw the same picture. The vendored PNGs are opaque and resvg v0.49.0
                // ignores the `path` attribute; testdata/golden/README.md has the evidence.
                {"with-path-and-xlink-href.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/donner-textPath-with-path.png")
                     .withReason("Reference disagreement: SVG 2 textPath path attribute, which "
                                 "resvg v0.49.0 ignores; the vendored PNG is opaque (#1175)")},
                {"with-path.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/donner-textPath-with-path.png")
                     .withReason("Reference disagreement: SVG 2 textPath path attribute, which "
                                 "resvg v0.49.0 ignores; the vendored PNG is opaque (#1175)")},
                // resvg v0.50.0 inverts arc length to within 0.5 user units, which leaves its
                // glyphs 0.36 px mean and 0.82 px max from Donner's arc-length placement; Geode
                // keeps that independent reference, a copy of the release PNG, within the default.
                // The explicit canvas keeps a regenerated golden at the 300x300 it was made at.
                {"closed-path.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/donner-textPath-closed-path.png",
                     kDefaultThreshold,
                     "Reference disagreement: resvg places glyph midpoints by inverting arc length "
                     "to within 0.5 user units; SVG 2 places them at their distance along the path")
                     .withGeodeGoldenOverride(
                         "donner/svg/renderer/testdata/golden/resvg-closed-path.png")
                     .setCanvasSize(300, 300)},
                {"complex.svg",
                 Params::UndefinedBehavior("reference is the resvg UB marker image; vertical text "
                                           "on a circular path")},
                {"dy-with-tiny-coordinates.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-dy-with-tiny-coordinates.png")
                     .withReason("Reference disagreement: the vendored PNG predates resvg's "
                                 "current dy layout on a path and places the glyphs differently; "
                                 "independent resvg 0.47.0 reference (golden README)")},
                // The vendored PNG is byte-identical to simple-case.png (align placement), so it
                // does not depict method="stretch". The golden is Donner's own rendering, a
                // regression guard; testdata/golden/README.md records its evidence and
                // regeneration.
                {"method=stretch.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-method=stretch.png")
                     .withReason("Reference disagreement: the vendored PNG is align placement")},
                {"mixed-children-1.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-mixed-children-1.png")
                     .withReason("Reference disagreement: SVG 2 continues at the path end point")},
                // SVG 2 continues the text after a textPath from the path's end point; the
                // vendored PNG continues from the end of the glyphs on the path, so the golden is
                // Donner's own rendering; testdata/golden/README.md records its evidence.
                {"mixed-children-2.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-mixed-children-2.png")
                     .withReason("Reference disagreement: SVG 2 continues at the path end point")},
                {"side=right.svg",
                 Params::WithThreshold(0.05f, kDefaultMismatchedPixels,
                                       "Gray guide and half-turned glyph boundary coverage")},
                {"tspan-with-absolute-position.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-tspan-with-absolute-position.png")
                     .withReason("Regression golden for absolute distance along the text path")},
                {"very-long-text.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-very-long-text.png")
                     .withReason("Reference disagreement: SVG 2 continues at the path end point")},
                {"with-baseline-shift-and-rotate.svg",
                 Params::UndefinedBehavior("baseline-shift + rotate")},
                // Donner-rendered golden; see donner/svg/renderer/testdata/golden/README.md.
                {"with-filter.svg",
                 GeodeUnresolvedBlurGate(HeavyGlyphReferenceGolden(
                     "donner/svg/renderer/testdata/golden/donner-textPath-with-filter.png"))},
                {"with-underline.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-with-underline.png")
                     .withReason("Regression golden for the on-path underline")},
                // Upright glyphs take the font's vmtx vertical origin, 860 units below the cell
                // top. Donner-rendered golden; see donner/svg/renderer/testdata/golden/README.md.
                {"writing-mode=tb.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/donner-textPath-writing-mode-tb.png",
                     kDefaultThreshold,
                     "Reference disagreement: the PNG moves upright glyphs off the vmtx origin CSS "
                     "Writing Modes 3 5.1.1 requires (VerticalTextPathPrecision)")},
            })),
        ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextTref, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "text/tref",
            {
                {"link-to-a-non-SVG-element.svg", QuarterPixelEdgeReference()},
                {"nested.svg", QuarterPixelEdgeReference()},
                {"link-to-a-complex-text.svg",
                 Params::DeprecatedInSVG2("<tref> (removed in SVG 2)")},
                {"link-to-a-non-text-element.svg",
                 Params::DeprecatedInSVG2("<tref> (removed in SVG 2)")},
                {"link-to-an-external-file-element.svg",
                 Params::DeprecatedInSVG2("<tref> (removed in SVG 2)")},
                {"link-to-text.svg", Params::DeprecatedInSVG2("<tref> (removed in SVG 2)")},
                {"position-attributes.svg", Params::DeprecatedInSVG2("<tref> (removed in SVG 2)")},
                {"style-attributes.svg", Params::DeprecatedInSVG2("<tref> (removed in SVG 2)")},
                {"with-a-title-child.svg", Params::DeprecatedInSVG2("<tref> (removed in SVG 2)")},
                {"with-text.svg", Params::DeprecatedInSVG2("<tref> (removed in SVG 2)")},
                {"xml-space.svg", Params::DeprecatedInSVG2("<tref> (removed in SVG 2)")},
            })),
        ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextTspan, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/tspan",
                {
                    {"mixed-font-size.svg", QuarterPixelEdgeReference()},
                    {"mixed-xml-space-1.svg", QuarterPixelEdgeReference()},
                    {"mixed-xml-space-2.svg", QuarterPixelEdgeReference()},
                    {"mixed-xml-space-3.svg", QuarterPixelEdgeReference()},
                    {"mixed.svg", QuarterPixelEdgeReference()},
                    {"multiple-coordinates.svg", QuarterPixelEdgeReference()},
                    {"nested.svg", QuarterPixelEdgeReference()},
                    {"only-with-y.svg", QuarterPixelEdgeReference()},
                    {"outside-the-text.svg", QuarterPixelEdgeReference()},
                    {"pseudo-multi-line.svg", QuarterPixelEdgeReference()},
                    {"rotate-and-display-none.svg", QuarterPixelEdgeReference()},
                    {"rotate-on-child.svg", QuarterPixelEdgeReference()},
                    {"sequential.svg", QuarterPixelEdgeReference()},
                    {"style-override.svg", QuarterPixelEdgeReference()},
                    {"text-shaping-across-multiple-tspan-1.svg", QuarterPixelEdgeReference()},
                    {"text-shaping-across-multiple-tspan-2.svg", QuarterPixelEdgeReference()},
                    {"transform.svg", QuarterPixelEdgeReference()},
                    {"tspan-bbox-1.svg", QuarterPixelEdgeReference()},
                    {"with-clip-path.svg", QuarterPixelEdgeReference()},
                    {"with-dy.svg", QuarterPixelEdgeReference()},
                    {"with-mask.svg", GeodeTranslucentEdgeColorGate(Params())},
                    {"with-opacity.svg", QuarterPixelEdgeReference()},
                    {"with-x-and-y.svg", QuarterPixelEdgeReference()},
                    {"without-attributes.svg", QuarterPixelEdgeReference()},
                    {"xml-space-1.svg", QuarterPixelEdgeReference()},
                    {"xml-space-2.svg", QuarterPixelEdgeReference()},
                    {"bidi-reordering.svg", Params().onlyTextFull().withReason("Arabic shaping")},
                    // Donner-rendered golden; see donner/svg/renderer/testdata/golden/README.md.
                    {"with-filter.svg",
                     GeodeBlurredCrosshairGate(HeavyGlyphReferenceGolden(
                         "donner/svg/renderer/testdata/golden/donner-tspan-with-filter.png"))},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextUnicodeBidi, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/unicode-bidi",
                {
                    {"bidi-override.svg",
                     Params().onlyTextFull().withReason("Arabic shaping needs HarfBuzz")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextWordSpacing, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory("text/word-spacing",
                                        {
                                            {"-5.svg", QuarterPixelEdgeReference()},
                                            {"0.svg", QuarterPixelEdgeReference()},
                                            {"10.svg", QuarterPixelEdgeReference()},
                                            {"2mm.svg", QuarterPixelEdgeReference()},
                                            {"5percent.svg", QuarterPixelEdgeReference()},
                                            {"normal.svg", QuarterPixelEdgeReference()},
                                            {"large-negative.svg",
                                             Params::UndefinedBehavior("word-spacing=-10000")},
                                        })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextWritingMode, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/writing-mode",
                {
                    {"horizontal-tb.svg", QuarterPixelEdgeReference()},
                    {"invalid-value.svg", QuarterPixelEdgeReference()},
                    {"lr-tb.svg", QuarterPixelEdgeReference()},
                    {"lr.svg", QuarterPixelEdgeReference()},
                    {"on-tspan.svg", QuarterPixelEdgeReference()},
                    {"rl-tb.svg", QuarterPixelEdgeReference()},
                    {"rl.svg", QuarterPixelEdgeReference()},
                    {"vertical-lr.svg", QuarterPixelEdgeReference()},
                    {"vertical-rl.svg", QuarterPixelEdgeReference()},
                    {"arabic-with-rl.svg",
                     Params().onlyTextFull().withReason("Arabic shaping needs HarfBuzz")},
                    {"japanese-with-tb.svg",
                     Params::WithGoldenOverride(
                         "donner/svg/renderer/testdata/golden/resvg-japanese-with-tb.png")
                         .onlyTextFull()
                         .withReason("Reference disagreement: the PNG places upright glyphs 5.6 px "
                                     "below the vmtx origin CSS Writing Modes 3 5.1.1 requires "
                                     "(golden README)")},
                    // CSS Text Decoration 3 leaves the position and thickness of an `auto`
                    // underline to the user agent, and the underline is most of the difference from
                    // any reference.
                    {"mixed-languages-with-tb-and-underline.svg",
                     Params::UndefinedBehavior(
                         "underline position and thickness are user-agent defined (CSS Text "
                         "Decoration 3)")},
                    {"mixed-languages-with-tb.svg",
                     Params::WithGoldenOverride(
                         "donner/svg/renderer/testdata/golden/resvg-mixed-languages-with-tb.png")
                         .onlyTextFull()
                         .withReason("Reference disagreement: upright glyphs 0.4 px below the vmtx "
                                     "origin CSS Writing Modes 3 5.1.1 requires (golden README)")},
                    {"tb-and-punctuation.svg",
                     Params::WithGoldenOverride(
                         "donner/svg/renderer/testdata/golden/resvg-tb-and-punctuation.png")
                         .onlyTextFull()
                         .withReason("Reference disagreement: the PNG lacks the `vert` punctuation "
                                     "forms and vmtx origin CSS Writing Modes 3 5.1.1 requires "
                                     "(golden README)")},
                    {"tb-with-rotate-and-underline.svg",
                     Params::UndefinedBehavior("tb with rotate and underline")},
                    {"tb-with-rotate.svg", Params::UndefinedBehavior("tb with rotate")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

}  // namespace donner::svg
