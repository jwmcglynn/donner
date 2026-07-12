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

// Disable only the Geode backend for a test the CPU backends still compare. Used for
// CPU-only features Geode does not implement yet (e.g. paint-order, 0-N dash caps).
ImageComparisonParams GeodeDisabled(std::string_view reason) {
  ImageComparisonParams params;
  params.disableBackend(RendererBackend::Geode, reason);
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

    // Canvas size matches the resvg-test-suite reference renderings. This is a
    // base param (mode-independent), so it stays on `test.params`.
    test.params.setCanvasSize(500, 500);

    testPlan.emplace_back(std::move(test));
  }

  std::sort(testPlan.begin(), testPlan.end());
  return testPlan;
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

  SVGDocument document = loadSVG(testcase.svgFilename.string().c_str(),
                                 Runfiles::instance().Rlocation("third_party/resvg-test-suite/"));
  renderAndCompare(document, testcase.svgFilename, goldenFilename.string().c_str());
}

INSTANTIATE_TEST_SUITE_P(
    FiltersEnableBackground, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "filters/enable-background", {},
                Params::RenderOnly("Deprecated SVG 1.1 enable-background / BackgroundImage"))),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(FiltersFeBlend, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("filters/feBlend")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFeColorMatrix, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "filters/feColorMatrix",
            {
                {"type=hueRotate-without-an-angle.svg",
                 Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "hueRotate(0) identity")},
                {"type=hueRotate.svg",
                 Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "hueRotate(30)")},
                {"type=matrix-with-empty-values.svg",
                 Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "identity matrix")},
                {"type=matrix-with-non-normalized-values.svg",
                 Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "non-normalized values")
                     .withGeodeGoldenOverride(
                         "donner/svg/renderer/testdata/golden/geode/"
                         "filters_feColorMatrix_type=matrix-with-non-normalized-values.png",
                         "Geode renders the gradient byte-identically to tiny-skia (verified), but "
                         "its 8-bit premultiplied filter intermediates, un-premultiplied at the "
                         "rect's edge-AA columns and amplified ~100x by the -100 matrix "
                         "coefficient, diverge from tiny-skia's float pipeline in thin 1px "
                         "vertical "
                         "bands at the gradient stop transitions. Geode's output is correct; only "
                         "the pathological amplification of sub-LSB precision differs.")},
                {"type=matrix-with-not-enough-values.svg",
                 Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "identity matrix")},
                {"type=matrix-with-too-many-values.svg",
                 Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "identity matrix")},
                {"type=matrix-without-values.svg",
                 Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "identity matrix")},
                {"type=matrix.svg",
                 Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "type=matrix")},
                {"type=saturate-with-a-large-coefficient.svg",
                 Params::RenderOnly("saturate 99999 (UB)")},
                {"type=saturate-with-negative-coefficient.svg",
                 Params::RenderOnly("saturate -0.5 (UB)")},
                {"type=saturate-without-a-coefficient.svg",
                 Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "identity saturate")},
                {"type=saturate.svg",
                 Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "saturate")},
                {"without-attributes.svg",
                 Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "no attrs identity")},
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

INSTANTIATE_TEST_SUITE_P(
    FiltersFeConvolveMatrix, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "filters/feConvolveMatrix",
                {
                    {"bias=-0.5.svg", Params::RenderOnly("UB: bias=-0.5")},
                    {"bias=0.5.svg", Params::RenderOnly("UB: bias=0.5")},
                    {"bias=9999.svg", Params::RenderOnly("UB: bias=9999")},
                    // The convolve math (window, 180° flip, target offset, divisor,
                    // preserveAlpha, linearRGB) is byte-equivalent to tiny-skia. The
                    // remaining Geode diff is a ~1px positional band per convolved
                    // feature, traced to the opacity-0.75 pattern *input* edge coverage
                    // (slug_fill 4-sample vs tiny-skia analytic), which the kernel
                    // spreads above threshold - the same root cause as the structure/svg
                    // and feImage/svg parity gaps. Not the color-space round-trip.
                    {"edgeMode=wrap-with-matrix-larger-than-target.svg",
                     Params::RenderOnly("UB: wrap with oversized kernel")},
                    {"edgeMode=wrap.svg",
                     Params::WithThreshold(kDefaultThreshold, 200,
                                           "Minor algorithm differences on edge handling (180px)")},
                    {"kernelMatrix-with-zero-sum-and-no-divisor.svg",
                     Params::RenderOnly("MatrixConvolution edge shift vs golden")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFeDiffuseLighting, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "filters/feDiffuseLighting",
                {
                    {"complex-transform.svg",
                     Params::WithThreshold(0.2f, kDefaultMismatchedPixels,
                                           "Shading differences, donner is smoother")},
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

INSTANTIATE_TEST_SUITE_P(
    FiltersFeDropShadow, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "filters/feDropShadow",
                {
                    {"only-stdDeviation.svg",
                     Params::WithThreshold(0.04f, 160, "Minor blur diffs")},
                    {"with-flood-color.svg", Params::WithThreshold(0.03f, 160, "Minor blur diffs")},

                    {"with-percent-offset.svg", Params::Skip("Bug: feDropShadow edge case")},
                })),
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
                     Params::WithThreshold(0.03f, kDefaultMismatchedPixels, "Minor AA differences")
                         .withGeodeGoldenOverride(
                             "donner/svg/renderer/testdata/golden/geode/"
                             "filters_feGaussianBlur_complex-transform.png",
                             "Geode's analytic coverage on this rotated rect is geometrically "
                             "exact (area matches a 16x scan-conversion to <0.3px, no missed "
                             "content); resvg's finite-sample scan-converter differs at the edge, "
                             "and the directional stdDeviation=\"12 0\" blur widens that into a "
                             "thin ~1px band (259px). Not a coverage bug; TinyGolden still checks "
                             "tiny-skia vs resvg.")},
                    {"huge-stdDeviation.svg",
                     Params::RenderOnly("Extreme sigma=1000; output is implementation-defined")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFeImage, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "filters/feImage",
            {
                {"svg.svg",
                 Params::WithGoldenOverride("donner/svg/renderer/testdata/golden/resvg-svg.png")
                     .withReason("We render higher quality")
                     .withGeodeGoldenOverride(
                         "donner/svg/renderer/testdata/golden/geode/filters_feImage_svg.png",
                         "feImage rasterizes the nested SVG to the same source bitmap for both "
                         "backends (shared imageData); Geode then resamples it with GPU "
                         "bilinear filtering, whose 1px edge band differs from tiny-skia's CPU "
                         "resampler. Geode's content matches; only the resample edge differs.")},
                {"with-subregion-5.svg", Params::Skip("Subregion with rotation: filter")},

                {"with-x-y-and-protruding-subregion-1.svg",
                 Params::Skip("Bug: feImage edge cases / unsupported subregion combinations")},
                {"with-x-y-and-protruding-subregion-2.svg",
                 Params::Skip("Bug: feImage edge cases / unsupported subregion combinations")},
                {"with-x-y.svg",
                 Params::Skip("Bug: feImage edge cases / unsupported subregion combinations")},
            })),
        ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(FiltersFeMerge, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory(
                                     "filters/feMerge",
                                     {
                                         {"complex-transform.svg",
                                          Params::WithThreshold(0.15f, kDefaultMismatchedPixels,
                                                                "Minor blur shading differences")},
                                     })),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFeMorphology, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory("filters/feMorphology",
                                        {
                                            {"source-with-opacity.svg", Params{}},
                                        })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(FiltersFeOffset, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("filters/feOffset")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFePointLight, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory("filters/fePointLight",
                                        {
                                            {"complex-transform.svg",
                                             Params::WithThreshold(0.1f, 120,
                                                                   "Minor shading differences")},
                                        })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFeSpecularLighting, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "filters/feSpecularLighting",
                {
                    {"with-fePointLight.svg",
                     Params::WithGoldenOverride(
                         "donner/svg/renderer/testdata/golden/resvg-with-fePointLight.png", 0.02f)
                         .withReason("resvg golden")},
                })),
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
                         .withReason("resvg bug: SpotLight Y")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFeTile, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory("filters/feTile",
                                        {
                                            {"complex-transform.svg",
                                             Params::RenderOnly("UB: complex transform")},
                                        })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFeTurbulence, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "filters/feTurbulence",
                {
                    {"color-interpolation-filters=sRGB.svg",
                     Params::WithThreshold(0.05f, kDefaultMismatchedPixels,
                                           "Minor shading differences")},
                    {"complex-transform.svg", Params::WithThreshold(0.05f, kDefaultMismatchedPixels,
                                                                    "Minor shading differences")},
                    {"stitchTiles=stitch.svg", Params::RenderOnly("UB: stitchTiles=stitch")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFilter, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "filters/filter",
            {
                {"complex-order-and-xlink-href.svg",
                 Params::Skip("Bug: Color is slightly off, we are missing transparency")},
                {"in=BackgroundAlpha-with-enable-background.svg",
                 Params::Skip("in=BackgroundAlpha (deprecated SVG 1.1)")},
                {"in=BackgroundImage-with-enable-background.svg",
                 Params::Skip("in=BackgroundImage (deprecated SVG 1.1)")},
                {"in=FillPaint-on-g-without-children.svg",
                 Params::RenderOnly("UB: in=FillPaint on empty group")},
                {"in=FillPaint-with-gradient.svg", Params::RenderOnly("UB: in=FillPaint gradient")},
                {"in=FillPaint-with-pattern.svg", Params::RenderOnly("UB: in=FillPaint pattern")},
                {"in=FillPaint-with-target-on-g.svg",
                 Params::RenderOnly("UB: in=FillPaint on group")},
                {"in=FillPaint.svg", Params::RenderOnly("UB: in=FillPaint")},
                {"in=StrokePaint.svg", Params::RenderOnly("UB: in=StrokePaint")},
                {"on-the-root-svg.svg", Params::RenderOnly("UB: Filter on the root `svg`")},
                {"transform-on-shape-with-filter-region.svg",
                 Params::Skip("Bug: We don't blur the right edge")},
                {"subregion-and-primitiveUnits=objectBoundingBox-1.svg",
                 Params::WithThreshold(0.3f, 600, "Subregion edge AA")},
                {"subregion-and-primitiveUnits=objectBoundingBox-2.svg",
                 Params::WithThreshold(0.3f, 600, "Subregion edge AA")},
                {"with-subregion-3.svg", Params::WithThreshold(0.1f, kDefaultMismatchedPixels,
                                                               "Minor shading differences")},
                {"content-outside-the-canvas-2.svg",
                 Params::Skip(
                     "Bug: <filter> edge cases (filterRes, filterUnits, multiple inputs)")},
                {"in=BackgroundAlpha.svg",
                 Params::Skip(
                     "Bug: <filter> edge cases (filterRes, filterUnits, multiple inputs)")},
                {"with-mask-on-parent.svg",
                 Params::Skip(
                     "Bug: <filter> edge cases (filterRes, filterUnits, multiple inputs)")},
                {"with-transform-outside-of-canvas.svg",
                 Params::Skip(
                     "Bug: <filter> edge cases (filterRes, filterUnits, multiple inputs)")},
            })),
        ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

// TODO: The filters/filter-functions category has CSS filter-function pixel-parity gaps when
// enabled wholesale. Keep the category disabled until those rendering mismatches are triaged; the
// previous resource-loading "Data corrupted" warnings are covered by ResourceManagerContext tests,
// and filters/feImage/empty.svg now runs in the active FiltersFeImage suite.
//
// INSTANTIATE_TEST_SUITE_P(
//     FiltersFilterFunctions, ImageComparisonTestFixture,
//     Combine(ValuesIn(getTestsInCategory("filters/filter-functions")),
//             ValuesIn(ActiveComparisonModes())),
//     TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFloodColor, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory("filters/flood-color",
                                        {
                                            {"inheritance-3.svg",
                                             Params::Skip("230K diff: ICC color")},
                                        })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(FiltersFloodOpacity, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("filters/flood-opacity")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    MaskingClip, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory("masking/clip",
                                        {
                                            {"simple-case.svg",
                                             Params::Skip("Not impl: CSS2 `clip` property "
                                                          "(rect() clipping, deprecated)")},
                                        })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(MaskingClipRule, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("masking/clip-rule")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    MaskingClipPath, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "masking/clipPath",
            {
                {"clip-path-on-children.svg", Params::Skip("Bug: Nested clip-path not working")},
                {"clip-path-with-transform-on-text.svg",
                 Params::Skip("Not impl: clipPath on <text>")},
                {"clipping-with-complex-text-1.svg",
                 Params::Skip("Not impl: clipPath with <text> children")},
                {"clipping-with-complex-text-2.svg",
                 Params::Skip("Not impl: clipPath with <text> children")},
                {"clipping-with-complex-text-and-clip-rule.svg",
                 Params::Skip("Not impl: clipPath with <text> children")},
                {"clipping-with-text.svg", Params::Skip("Not impl: clipPath with <text> children")},
                {"on-the-root-svg-without-size.svg",
                 Params::RenderOnly("UB: on root `<svg>` without size")},
                {"with-use-child.svg", Params::Skip("Not impl: <use> child")},

                {"circle-shorthand-with-stroke-box.svg",
                 Params::Skip("Bug: clipPath edge cases beyond core support")},
                {"circle-shorthand-with-view-box.svg",
                 Params::Skip("Bug: clipPath edge cases beyond core support")},
                {"circle-shorthand.svg",
                 Params::Skip("Bug: clipPath edge cases beyond core support")},
            })),
        ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    MaskingMask, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "masking/mask",
                {
                    {"color-interpolation=linearRGB.svg",
                     Params::Skip("Not implemented: color-interpolation linearRGB")},
                    {"recursive-on-child.svg", Params::RenderOnly("UB: Recursive on child")},
                    {"on-a-small-object.svg", WithMaxPixels(120, "Small mask edge AA")},
                    {"with-image.svg",
                     Params::WithThreshold(0.1f, kDefaultMismatchedPixels,
                                           "Mask with <image> (bilinear edge diffs)")},

                    {"half-width-region-with-rotation.svg",
                     Params::Skip(
                         "Bug: mask edge cases (color-interpolation, mask-units, mask-type) need "
                         "investigation")},
                    {"mask-on-self-with-mask-type=alpha.svg",
                     Params::Skip(
                         "Bug: mask edge cases (color-interpolation, mask-units, mask-type) need "
                         "investigation")},
                    {"mask-on-self-with-mixed-mask-type.svg",
                     Params::Skip(
                         "Bug: mask edge cases (color-interpolation, mask-units, mask-type) need "
                         "investigation")},
                    {"mask-type-in-style.svg",
                     Params::Skip("Bug: mask edge cases (color-interpolation, "
                                  "mask-units, mask-type) need investigation")},
                    {"mask-type=alpha.svg",
                     Params::Skip("Bug: mask edge cases (color-interpolation, "
                                  "mask-units, mask-type) need investigation")},
                    {"on-group-with-transform.svg",
                     Params::Skip(
                         "Bug: mask edge cases (color-interpolation, mask-units, mask-type) need "
                         "investigation")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintServersLinearGradient, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory("paint-servers/linearGradient",
                                        {
                                            {"invalid-gradientTransform.svg",
                                             Params::RenderOnly("UB: Invalid `gradientTransform`")},
                                        })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintServersPattern, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "paint-servers/pattern",
                {
                    {"invalid-patternTransform.svg",
                     Params::RenderOnly("UB: Invalid patternTransform")},
                    {"out-of-order-referencing.svg",
                     Params::WithThreshold(0.6f, 800, "Nested pattern AA (768px)")},
                    {"overflow=visible.svg", Params::RenderOnly("UB: overflow=visible")},
                    {"pattern-on-child.svg", Params::WithThreshold(0.2f, kDefaultMismatchedPixels,
                                                                   "Anti-aliasing artifacts")},
                    {"patternContentUnits-with-viewBox.svg",
                     Params::WithThreshold(kDefaultThreshold, 150, "Pattern AA drift")},
                    {"patternContentUnits=objectBoundingBox.svg",
                     Params::WithThreshold(kDefaultThreshold, 250, "Pattern AA drift")},
                    {"recursive-on-child.svg",
                     Params::WithThreshold(0.2f, kDefaultMismatchedPixels,
                                           "Larger threshold due to recursive pattern seams.")},
                    {"self-recursive-on-child.svg",
                     Params::WithThreshold(0.2f, kDefaultMismatchedPixels,
                                           "Larger threshold due to recursive pattern seams.")},
                    {"self-recursive.svg",
                     Params::WithThreshold(0.2f, kDefaultMismatchedPixels,
                                           "Larger threshold due to recursive pattern seams.")},
                    {"text-child.svg",
                     Params::WithThreshold(0.5f, 1150, "AA artifacts + quad glyph outlines")},
                    {"tiny-pattern-upscaled.svg",
                     Params::WithThreshold(0.02f, 500, "Upscaled pattern edge AA")},
                    {"transform-and-patternTransform.svg",
                     ImageComparisonParams().disableBackend(
                         RendererBackend::Geode,
                         "Geode analytic dual-ray Slug coverage (0041 §6) on the rotated rounded "
                         "rect edge diverges from the resvg finite-sample reference by a ~1px edge "
                         "band (115px), interacting with the pattern tile seams")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintServersRadialGradient, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "paint-servers/radialGradient",
                {
                    {"fr=-1.svg", Params::RenderOnly("UB: fr=-1 (SVG 2)")},
                    {"fr=0.5.svg", Params::RenderOnly("UB: fr=0.5 (SVG 2)")},
                    {"fr=0.7.svg", Params::Skip("Test suite bug? fr > default value of")},
                    {"invalid-gradientTransform.svg",
                     Params::RenderOnly("UB: Invalid `gradientTransform`")},
                    {"invalid-gradientUnits.svg",
                     Params::RenderOnly("UB: Invalid `gradientUnits`")},
                    {"negative-r.svg", Params::RenderOnly("UB: Negative `r`")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintServersStop, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory("paint-servers/stop",
                                        {
                                            {"stop-color-with-inherit-1.svg",
                                             Params::Skip("Bug? Strange edge case, stop-color")},
                                        })),
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

INSTANTIATE_TEST_SUITE_P(
    PaintingContext, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "painting/context",
                {
                    {"with-pattern-objectBoundingBox-in-use.svg",
                     Params::WithGoldenOverride("donner/svg/renderer/testdata/golden/"
                                                "resvg-with-pattern-objectBoundingBox-in-use.png")
                         .withReason("resvg golden bakes fractional-tile rounding (56.57px tile "
                                     "rasterized as 57px, ~0.8% period drift); Donner tiles at "
                                     "the exact computed context box, see doc 0009")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintingDisplay, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory("painting/display",
                                        {
                                            {"bBox-impact.svg", WithMaxPixels(170, "Clip edge AA")},
                                            {"none-on-tref.svg", Params::Skip("Not impl: <tref>")},
                                        })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintingFill, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "painting/fill",
                {
                    {"icc-color.svg", Params::RenderOnly("UB: ICC color")},
                    {"linear-gradient-on-text.svg", Params::WithThreshold(kDefaultThreshold, 500)},
                    {"pattern-on-text.svg", Params::WithThreshold(kDefaultThreshold, 2100)},
                    {"radial-gradient-on-text.svg", Params::WithThreshold(kDefaultThreshold, 500)},
                    {"rgb-int-int-int.svg", Params::RenderOnly("UB: rgb(int int int)")},
                    {"valid-FuncIRI-with-a-fallback-ICC-color.svg",
                     Params::Skip("Not impl: Fallback with icc-color")},
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
                 Params::Skip("Not impl: image-rendering property (pixelated/crisp-edges/smooth)")},
                {"optimizeSpeed.svg",
                 Params::Skip("Not impl: image-rendering property (pixelated/crisp-edges/smooth)")},
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
                {"default-clip.svg", WithMaxPixels(300, "Marker clip edge AA")},
                {"marker-on-circle.svg", WithMaxPixels(160,
                                                       "Circle stroke edge: cross-GPU coverage "
                                                       "fringe, 122px on Mesa lavapipe vs golden")},
                {"orient=auto-on-M-C-C-4.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-orient=auto-on-M-C-C-4.png")
                     .withReason("Pre-existing rendering diff (stroke/AA), not cusp-related")},
                {"orient=auto-on-M-L-Z.svg", Params{}},
                {"target-with-subpaths-2.svg", Params::RenderOnly("UB: Target with subpaths")},
                {"with-a-large-stroke.svg", WithMaxPixels(300, "Marker clip edge AA")},
                {"with-a-text-child.svg",
                 Params::WithThreshold(kDefaultThreshold, 110, "Minor AA diffs on text_full")
                     .requireFeature(RendererBackendFeature::Text, "text rendering")},
                {"with-an-image-child.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-with-an-image-child.png")
                     .withMaxPixelsDifferent(350)
                     .withReason("Image child edge AA")},
                {"with-invalid-markerUnits.svg", WithMaxPixels(300, "Marker clip edge AA")},
                {"with-markerUnits=userSpaceOnUse.svg", WithMaxPixels(300, "Marker clip edge AA")},
                {"with-viewBox-1.svg", Params::RenderOnly("UB: with `viewBox`")},

                // Issue #623: the rounded-rect start corner now stacks marker-start +
                // arrival marker-mid + marker-end (3 markers), matching resvg, after
                // Path::vertices() emits the arrival mid for zero-length-close corner
                // contours. Residual is sub-marker edge coverage.
                {"marker-on-rounded-rect.svg", WithMaxPixels(60, "Marker stack edge coverage")},
                {"recursive-5.svg", Params{}},
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

INSTANTIATE_TEST_SUITE_P(
    PaintingPaintOrder, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "painting/paint-order",
            {
                // `Te<tspan paint-order="stroke fill">xt</tspan>`. NOT a positioning bug:
                // the glyph positions are correct (identical to the unsplit on-text.svg, which
                // passes - cross-tspan kerning keeps "xt" at the same x as "Text"). The ~1968px
                // residual is a paint-order *layering* difference across the two runs with
                // different paint-orders: Donner draws run "Te" (default order: fill then
                // stroke-on-top) fully, then run "xt" (stroke then fill) fully on top, so at the
                // e/x overlap "x"'s green fill covers "e"'s on-top blue stroke where resvg keeps
                // the blue (≈1.6k green↔blue edge swaps). resvg layers paint-order across the
                // whole <text> rather than per-run. Fixing this needs the renderer's text
                // paint-order passes to span runs, not the text layout. See #624.
                {"on-tspan.svg",
                 Params::Skip("paint-order layering across tspan runs (not positioning) - #624")},
                // paint-order rendering is implemented on the CPU backend only; Geode does not
                // honor the fill/stroke/marker reordering yet. Compare CPU, disable Geode.
                {"fill-markers-stroke.svg", GeodeDisabled("Geode paint-order rendering gap")},
                {"markers-stroke.svg", GeodeDisabled("Geode paint-order rendering gap")},
                {"markers.svg", GeodeDisabled("Geode paint-order rendering gap")},
                {"stroke-markers-fill.svg", GeodeDisabled("Geode paint-order rendering gap")},
                {"stroke-markers.svg", GeodeDisabled("Geode paint-order rendering gap")},
                {"stroke.svg", GeodeDisabled("Geode paint-order rendering gap")},
                {"on-text.svg", GeodeDisabled("Geode paint-order rendering gap")},
            })),
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
                    // Geode's analytic stroke coverage for this near-degenerate cubic is visibly
                    // close, but differs by 410 pixels on Linux aarch64.
                    {"control-points-clamping-1.svg",
                     WithMaxPixels(150, "Stroke control-point clamping edge coverage")
                         .withGeodeMaxPixelsDifferent(500)},
                    {"linear-gradient-on-text.svg",
                     Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "AA artifacts")
                         .requireFeature(RendererBackendFeature::Text, "text rendering")},
                    {"pattern-on-text.svg",
                     Params::WithThreshold(0.1f, kDefaultMismatchedPixels, "AA artifacts")},
                    {"radial-gradient-on-text.svg", Params::Skip("Bug: Gradient stroke on text")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintingStrokeDasharray, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "painting/stroke-dasharray",
                {
                    {"multiple-subpaths.svg",
                     Params::WithThreshold(0.13f, kDefaultMismatchedPixels,
                                           "Larger threshold due to anti-aliasing artifacts.")},
                    {"negative-sum.svg", Params::RenderOnly("UB (negative sum)")},
                    {"negative-values.svg", Params::RenderOnly("UB (negative values)")},

                    // `0 N` dash patterns (zero-length dashes -> caps/dots) now render. `40 0`
                    // (zero gap) differs by ~625px at exactly the closed-rect dash-START corner
                    // (doc (40,40), the outer top-left stroke quadrant). Root-caused (#623): the
                    // `<rect>` is a *closed* contour, so tiny-skia (faithful Rust port) seam-joins
                    // the first and last `40`-unit dash across the start vertex into one continuous
                    // dash - making that corner an interior MITER (filled quadrant). resvg's golden
                    // butt-caps it (notched) because usvg flattens the rect to a *non-closed* path
                    // before dashing. Donner's mitered closed-contour seam is the spec-conformant
                    // behavior (matches Skia/Chrome/Firefox); the residual is a resvg-pipeline
                    // (usvg path-normalization) difference, not a Donner/tiny-skia bug. Pinned by
                    // RendererTests.DashSeamClosedContourMitersStartCorner in Renderer_tests.cc.
                    {"n-0.svg",
                     Params::Skip("resvg golden butt-caps the closed-rect dash-start corner; "
                                  "Donner spec-correctly seam-joins it (usvg flattens rect to an "
                                  "open path). See DashSeamClosedContourMitersStartCorner.")},
                    // `0 N` round/square caps render as dots on the CPU backend; Geode's dot
                    // rendering for zero-length dashes differs. Compare CPU, disable Geode.
                    {"0-n-with-round-caps.svg", GeodeDisabled("Geode 0-N dash cap rendering gap")},
                    {"0-n-with-square-caps.svg", GeodeDisabled("Geode 0-N dash cap rendering gap")},
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
                    {"arcs.svg", Params::RenderOnly("UB (SVG 2), no UA supports `arcs`")},
                    {"miter-clip.svg",
                     Params::RenderOnly("UB (SVG 2), no UA supports `miter-clip`")},
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
    Combine(ValuesIn(getTestsInCategory("painting/stroke-width",
                                        {
                                            {"negative.svg",
                                             Params::RenderOnly("UB: Nothing should be rendered")},
                                        })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintingVisibility, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "painting/visibility",
                {
                    {"bbox-impact-3.svg",
                     Params::Skip("Not impl: <text> contributing to bbox handling")},
                    {"collapse-on-tspan.svg",
                     Params().requireFeature(RendererBackendFeature::Text, "text rendering")},
                    {"hidden-on-tspan.svg",
                     Params().requireFeature(RendererBackendFeature::Text, "text rendering")},
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

INSTANTIATE_TEST_SUITE_P(
    ShapesLine, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "shapes/line",
                {
                    {"no-x1-coordinate.svg", WithMaxPixels(120, "Line endpoint AA")},
                    {"no-y1-coordinate.svg", WithMaxPixels(120, "Line endpoint AA")},
                    {"simple-case.svg",
                     Params::WithThreshold(0.02f, kDefaultMismatchedPixels,
                                           "Larger threshold due to anti-aliasing")},
                })),
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

INSTANTIATE_TEST_SUITE_P(
    StructureDefs, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory("structure/defs",
                                        {
                                            {"style-inheritance-on-text.svg",
                                             Params::WithThreshold(kDefaultThreshold, 6500)},
                                        })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(StructureG, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("structure/g")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StructureImage, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "structure/image",
            {
                {"float-size.svg", Params::RenderOnly("UB: Float size")},
                {"no-height-on-svg.svg", Params::RenderOnly("UB: No height")},
                {"no-width-and-height-on-svg.svg", Params::RenderOnly("UB: No width and height")},
                {"no-width-on-svg.svg", Params::RenderOnly("UB: No width")},
                {"url-to-png.svg", Params::Skip("Not impl: External URLs")},
                {"url-to-svg.svg", Params::Skip("Not impl: External URLs")},

                // Golden kernel mismatch (0021 B3): these vendored goldens were generated by a
                // resvg whose <image> upscale kernel matches neither tiny-skia Bilinear nor
                // Bicubic (Mitchell). Donner's placement/sizing is verified correct (see the
                // LayoutSystem/RenderingContext unit tests); the residual is upscale-kernel-only.
                // Blocked on a vendored-golden refresh, not a Donner code change.
                {"embedded-gif.svg",
                 Params::Skip("Golden kernel mismatch: vendored golden matches neither tiny-skia "
                              "Bilinear nor Bicubic <image> upscaling (0021 B3)")},
                {"embedded-jpeg-without-mime.svg",
                 Params::Skip("Golden kernel mismatch: vendored golden matches neither tiny-skia "
                              "Bilinear nor Bicubic <image> upscaling (0021 B3)")},
                {"external-gif.svg",
                 Params::Skip("Golden kernel mismatch: vendored golden matches neither tiny-skia "
                              "Bilinear nor Bicubic <image> upscaling (0021 B3)")},
                {"no-height.svg",
                 Params::Skip("Golden kernel mismatch: vendored golden matches neither tiny-skia "
                              "Bilinear nor Bicubic <image> upscaling (0021 B3)")},
                {"no-width-and-height.svg",
                 Params::Skip("Golden kernel mismatch: vendored golden matches neither tiny-skia "
                              "Bilinear nor Bicubic <image> upscaling (0021 B3)")},
                {"no-width.svg",
                 Params::Skip("Golden kernel mismatch: vendored golden matches neither tiny-skia "
                              "Bilinear nor Bicubic <image> upscaling (0021 B3)")},
                {"preserveAspectRatio=none.svg",
                 Params::Skip("Golden kernel mismatch: vendored golden matches neither tiny-skia "
                              "Bilinear nor Bicubic <image> upscaling (0021 B3)")},
                {"preserveAspectRatio=xMaxYMax-meet.svg",
                 Params::Skip("Golden kernel mismatch: vendored golden matches neither tiny-skia "
                              "Bilinear nor Bicubic <image> upscaling (0021 B3)")},
                {"preserveAspectRatio=xMidYMid-meet.svg",
                 Params::Skip("Golden kernel mismatch: vendored golden matches neither tiny-skia "
                              "Bilinear nor Bicubic <image> upscaling (0021 B3)")},
                {"preserveAspectRatio=xMinYMin-meet.svg",
                 Params::Skip("Golden kernel mismatch: vendored golden matches neither tiny-skia "
                              "Bilinear nor Bicubic <image> upscaling (0021 B3)")},

                // Golden kernel mismatch, current-resvg flavor (0021 B3): these two goldens match
                // current resvg's Mitchell-bicubic <image> upscaling (verified: they pass when
                // Donner's drawImage uses tiny-skia FilterQuality::Bicubic). Donner upscales with
                // Bilinear, which the majority of this category's goldens require. Blocked on the
                // category-wide kernel decision + golden refresh.
                {"no-height-non-square.svg",
                 Params::Skip("Golden kernel mismatch: golden requires Mitchell-bicubic <image> "
                              "upscaling (current resvg); Donner uses bilinear (0021 B3)")},
                {"width-and-height-set-to-auto.svg",
                 Params::Skip("Golden kernel mismatch: golden requires Mitchell-bicubic <image> "
                              "upscaling (current resvg); Donner uses bilinear (0021 B3)")},

                {"embedded-svg-with-text.svg",
                 Params::Skip("Golden mismatch: resvg parses <image>-embedded SVGs with an empty "
                              "fontdb so its golden renders no text; Donner renders the text "
                              "(0021 B3)")},
            })),
        ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StructureStyle, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory("structure/style",
                                        {
                                            {"external-CSS.svg",
                                             Params::Skip("Not impl: CSS @import")},
                                            {"non-presentational-attribute.svg",
                                             Params::Skip("Not impl: <svg version=\"1.1\">")},
                                        })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StructureStyleAttribute, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "structure/style-attribute",
                {
                    {"non-presentational-attribute.svg",
                     Params::Skip("<svg version=\"1.1\"> disables geometry attributes in style")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StructureSvg, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "structure/svg",
                {
                    {"funcIRI-parsing.svg", Params::RenderOnly("UB: FuncIRI parsing")},
                    {"funcIRI-with-invalid-characters.svg",
                     Params::RenderOnly("UB: FuncIRI with invalid chars")},
                    {"invalid-id-attribute-1.svg", Params::RenderOnly("UB: Invalid id attribute")},
                    {"invalid-id-attribute-2.svg", Params::RenderOnly("UB: Invalid id attribute")},
                    {"no-size.svg", Params::Skip("Not impl: Computed bounds from content")},
                    {"not-UTF-8-encoding.svg", Params::Skip("Bug/Not impl? Non-UTF8 encoding")},
                    {"preserveAspectRatio-with-viewBox-not-at-zero-pos.svg",
                     Params::WithThreshold(0.13f, 500, "Viewport edge AA")},
                    {"preserveAspectRatio=none.svg",
                     Params::WithThreshold(0.13f, 1000, "Viewport edge AA")},
                    {"preserveAspectRatio=xMaxYMax-slice.svg",
                     Params::WithThreshold(0.13f, kDefaultMismatchedPixels,
                                           "Has anti-aliasing artifacts.")},
                    {"preserveAspectRatio=xMaxYMax.svg",
                     Params::WithThreshold(0.13f, 300, "Viewport edge AA")},
                    {"preserveAspectRatio=xMidYMid-slice.svg",
                     Params::WithThreshold(0.13f, kDefaultMismatchedPixels,
                                           "Has anti-aliasing artifacts.")},
                    {"preserveAspectRatio=xMidYMid.svg",
                     Params::WithThreshold(0.13f, 300, "Viewport edge AA")},
                    {"preserveAspectRatio=xMinYMin-slice.svg",
                     Params::WithThreshold(0.13f, kDefaultMismatchedPixels,
                                           "Has anti-aliasing artifacts.")},
                    {"preserveAspectRatio=xMinYMin.svg",
                     Params::WithThreshold(0.13f, kDefaultMismatchedPixels,
                                           "Has anti-aliasing artifacts.")},
                    {"proportional-viewBox.svg",
                     Params::WithThreshold(0.13f, kDefaultMismatchedPixels,
                                           "Has anti-aliasing artifacts.")},
                    {"rect-inside-a-non-SVG-element.svg",
                     Params::Skip("Bug? Rect inside unknown element")},
                    {"viewBox-not-at-zero-pos.svg",
                     Params::WithThreshold(0.13f, kDefaultMismatchedPixels,
                                           "Has anti-aliasing artifacts.")},
                    {"xmlns-validation.svg", Params::Skip("Bug? xmlns validation")},

                    {"funcIRI-with-quotes.svg", Params::Skip("Bug: <svg> root element edge cases")},
                    {"nested-svg-one-with-rect-and-one-with-viewBox.svg",
                     Params::Skip("Bug: <svg> root element edge cases")},
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

INSTANTIATE_TEST_SUITE_P(
    StructureTransform, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "structure/transform",
                {
                    {"rotate-at-position.svg",
                     Params::WithThreshold(0.05f, kDefaultMismatchedPixels,
                                           "Larger threshold due to anti-aliasing artifacts.")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StructureTransformOrigin, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory("structure/transform-origin", {},
                                        Params::WithThreshold(0.13f, 300, "Rotated edge AA"))),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StructureUse, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory("structure/use",
                                        {
                                            {"xlink-to-an-external-file.svg",
                                             Params::Skip("Not impl: External file.")},
                                        })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextAlignmentBaseline, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/alignment-baseline",
                {
                    // UB: resvg's golden PNG is a "UB" placeholder overlay, so no render can
                    // match it. Render for no-crash coverage only.
                    {"after-edge.svg", Params::RenderOnly("UB: golden is a UB placeholder image")},
                    {"baseline.svg", Params::RenderOnly("UB: golden is a UB placeholder image")},
                    {"ideographic.svg", Params::RenderOnly("UB: golden is a UB placeholder image")},
                    {"text-after-edge.svg",
                     Params::RenderOnly("UB: golden is a UB placeholder image")},

                    {"hanging-on-vertical.svg",
                     Params::Skip("Bug: mixed-script vertical flow (upright CJK + rotated Latin) "
                                  "column geometry differs from resvg; alignment-baseline is "
                                  "correctly ignored in vertical mode. Same gap as the "
                                  "text/writing-mode mixed-languages-with-tb skip.")},
                    {"two-textPath-with-middle-on-first.svg",
                     Params().withMaxPixelsDifferent(200).withReason(
                         "Sub-pixel glyph placement along the two paths; ~108 scattered "
                         "edge pixels, no positional drift (bounding boxes match exactly)")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextBaselineShift, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory("text/baseline-shift",
                                        {
                                            {"nested-with-baseline-1.svg",
                                             Params::WithThreshold(0.1f, kDefaultMismatchedPixels,
                                                                   "Minor AA artifacts on axis")},
                                            {"nested-with-baseline-2.svg",
                                             Params::WithThreshold(0.1f, kDefaultMismatchedPixels,
                                                                   "Minor AA artifacts on axis")},
                                        })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextDirection, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/direction",
                {
                    {"rtl-with-vertical-writing-mode.svg",
                     Params::Skip("Not impl: direction property (BiDi text shaping)")},
                    {"rtl.svg", Params::Skip("Not impl: direction property (BiDi text shaping)")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextDominantBaseline, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/dominant-baseline",
                {
                    // UB: resvg's golden PNG is a "UB" placeholder overlay, so no render can
                    // match it. Render for no-crash coverage only.
                    {"reset-size.svg", Params::RenderOnly("UB: golden is a UB placeholder image")},
                    {"use-script.svg", Params::RenderOnly("UB: golden is a UB placeholder image")},

                    {"hanging.svg",
                     Params::WithThreshold(
                         0.1f, kDefaultMismatchedPixels,
                         "Golden's 0.5px crosshair hairline is rasterized with a sub-pixel x "
                         "offset (vertical-line alpha 128/192 vs 160/160 in the sibling "
                         "middle/central/alphabetic goldens); the glyph itself matches")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextFont, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/font",
                {
                    {"simple-case.svg", Params::Skip("Canvas size mismatch (400 vs 500)")},

                    {"font-shorthand.svg", Params::Skip("Not impl: font shorthand property")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextFontFamily, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "text/font-family",
            {
                {"bold-sans-serif.svg", Params::WithThreshold(kDefaultThreshold, 5200,
                                                              "Bold sans-serif (Noto Sans Bold)")},
                {"cursive.svg",
                 Params::WithThreshold(kDefaultThreshold, 5000, "cursive (Yellowtail)")},
                {"fallback-1.svg", Params::Skip("Fallback from invalid family (different")},
                {"fallback-2.svg", Params::WithThreshold(kDefaultThreshold, 1000,
                                                         "Fallback list: \"Invalid, Noto Sans\"")},
                {"fantasy.svg",
                 Params::WithThreshold(kDefaultThreshold, 5200, "fantasy (Sedgwick Ave Display)")},
                {"font-list.svg", Params::WithThreshold(kDefaultThreshold, 1300,
                                                        "Font list: Source Sans Pro fallback")},
                {"monospace.svg",
                 Params::WithThreshold(kDefaultThreshold, 600, "monospace (Noto Mono)")},
                {"sans-serif.svg",
                 Params::WithThreshold(kDefaultThreshold, 1900, "sans-serif (Noto Sans)")},
                {"serif.svg", Params::WithThreshold(kDefaultThreshold, 4200, "serif (Noto Serif)")},
                {"source-sans-pro.svg",
                 Params::WithThreshold(kDefaultThreshold, 1300, "Source Sans Pro")},
            })),
        ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextFontKerning, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/font-kerning",
                {
                    {"arabic-script.svg",
                     Params::Skip("Not impl: font-kerning property (HarfBuzz feature toggle)")},
                    {"none.svg",
                     Params::Skip("Not impl: font-kerning property (HarfBuzz feature toggle)")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextFontSize, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "text/font-size",
            {
                {"named-value-without-a-parent.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-named-value-without-a-parent.png")
                     .withReason("Donner uses CSS Fonts Level 4, which has")},
                {"named-value.svg", Params::WithGoldenOverride(
                                        "donner/svg/renderer/testdata/golden/resvg-named-value.png")
                                        .withReason("Donner uses CSS Fonts Level 4, which has")},
                {"negative-size.svg", Params::RenderOnly("UB: negative font size")},
            })),
        ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextFontSizeAdjust, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory("text/font-size-adjust",
                                        {
                                            {"simple-case.svg",
                                             Params::Skip("Not impl: font-size-adjust property")},
                                        })),
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
                    {"inherit.svg", Params().withSimpleTextMaxPixels(1200).withReason(
                                        "small-caps is emulated with simple text")},
                    {"small-caps.svg", Params().withSimpleTextMaxPixels(1200).withReason(
                                           "small-caps is emulated with simple text")},
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
                     Params::Skip("Not impl: glyph-orientation-horizontal (deprecated SVG 1.1)")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextGlyphOrientationVertical, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/glyph-orientation-vertical",
                {
                    {"simple-case.svg",
                     Params::Skip("Not impl: glyph-orientation-vertical (deprecated SVG 1.1)")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextKerning, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/kerning",
                {
                    {"0.svg", Params::Skip("Not impl: kerning attribute (deprecated SVG 1.1)")},
                    {"10percent.svg",
                     Params::Skip("Not impl: kerning attribute (deprecated SVG 1.1)")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextLengthAdjust, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/lengthAdjust",
                {
                    {"text-on-path.svg",
                     Params::Skip("Not impl: lengthAdjust attribute (parented to textLength)")},
                    {"vertical.svg",
                     Params::Skip("Not impl: lengthAdjust attribute (parented to textLength)")},
                    {"with-underline.svg",
                     Params::Skip("Not impl: lengthAdjust attribute (parented to textLength)")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextLetterSpacing, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/letter-spacing",
                {
                    {"large-negative.svg", Params::RenderOnly("UB: negative letter-spacing")},
                    {"mixed-scripts.svg",
                     Params::Skip("Needs BiDi: mixed LTR Latin + RTL Arabic in one span")},
                    {"non-ASCII-character.svg",
                     Params::Skip("Bug? We render with a different CJK glyph. Wrong font?")},
                    {"on-Arabic.svg", Params()
                                          .requireFeature(RendererBackendFeature::TextFull)
                                          .withReason("Arabic text")},

                    {"filter-bbox.svg", Params::Skip("Bug: letter-spacing edge cases")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextText, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/text",
                {
                    {"bidi-reordering.svg", Params::Skip("Not impl: Bidirectional text shaping")},
                    {"complex-grapheme-split-by-tspan.svg",
                     Params::RenderOnly("UB: grapheme split by tspan")},
                    {"complex-graphemes-and-coordinates-list.svg",
                     Params::WithGoldenOverride("donner/svg/renderer/testdata/golden/"
                                                "resvg-complex-graphemes-and-coordinates-list.png")
                         .onlyTextFull()
                         .withReason("Simple text can't compose combining marks")},
                    {"complex-graphemes.svg",
                     Params().onlyTextFull().withReason("Combining mark needs HarfBuzz")},
                    {"compound-emojis-and-coordinates-list.svg",
                     Params::WithGoldenOverride("donner/svg/renderer/testdata/golden/"
                                                "resvg-compound-emojis-and-coordinates-list.png",
                                                0.1f)
                         .withMaxPixelsDifferent(1100)
                         .onlyTextFull()
                         .withReason("Emoji bitmap scaling differs from the golden")},
                    {"compound-emojis.svg",
                     Params::WithThreshold(0.2f, kDefaultMismatchedPixels,
                                           "Emoji, differences between resvg and our bitmap")
                         .onlyTextFull()},
                    {"emojis.svg", Params::WithThreshold(0.2f, kDefaultMismatchedPixels,
                                                         "Emoji, differences between")
                                       .onlyTextFull()},
                    {"fill-rule=evenodd.svg",
                     Params().onlyTextFull().withReason("Arabic text shaping requires text-full")},
                    {"rotate-on-Arabic.svg",
                     Params::WithGoldenOverride(
                         "donner/svg/renderer/testdata/golden/resvg-rotate-on-Arabic.png")
                         .onlyTextFull()
                         .withReason("Arabic text shaping requires text-full,")},
                    {"rotate-with-multiple-values-and-complex-text.svg",
                     Params().onlyTextFull().withReason("Complex diatrics requires text-full")},
                    {"x-and-y-with-multiple-values-and-arabic-text.svg",
                     Params::WithGoldenOverride(
                         "donner/svg/renderer/testdata/golden/"
                         "resvg-x-and-y-with-multiple-values-and-arabic-text.png")
                         .withMaxPixelsDifferent(400)
                         .onlyTextFull()
                         .withReason("Arabic text shaping; vertical-axis AA diff not "
                                     "the focus of the test")},
                    {"xml-lang=ja.svg", Params::WithThreshold(kDefaultThreshold, 19100)},
                    {"xml-space.svg", Params::WithThreshold(kDefaultThreshold, 1400)},
                    {"zalgo.svg", Params().withMaxPixelsDifferent(300).onlyTextFull().withReason(
                                      "Complex diacritics; vertical-axis AA diff "
                                      "not the focus of the test")},

                    {"filter-bbox.svg",
                     Params::Skip(
                         "Bug: text rendering edge cases (mixed inline content, BiDi-adjacent)")},
                    {"ligatures-handling-in-mixed-fonts-1.svg",
                     Params::Skip(
                         "Bug: text rendering edge cases (mixed inline content, BiDi-adjacent)")},
                    {"ligatures-handling-in-mixed-fonts-2.svg",
                     Params::Skip(
                         "Bug: text rendering edge cases (mixed inline content, BiDi-adjacent)")},
                    {"real-text-height.svg",
                     Params::Skip(
                         "Bug: text rendering edge cases (mixed inline content, BiDi-adjacent)")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextTextAnchor, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/text-anchor",
                {
                    {"coordinates-list.svg",
                     Params::WithThreshold(0.1f, kDefaultMismatchedPixels, "Axis AA artifacts")},
                    {"on-tspan-with-arabic.svg",
                     Params().requireFeature(RendererBackendFeature::TextFull)},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextTextDecoration, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "text/text-decoration",
            {
                {"all-types-inline-comma-separated.svg",
                 Params::WithThreshold(0.1f, kDefaultMismatchedPixels, "Minor AA diffs")},
                {"all-types-inline-no-spaces.svg",
                 Params::WithThreshold(0.1f, kDefaultMismatchedPixels, "Minor AA diffs")},
                {"all-types-inline.svg",
                 Params::WithThreshold(0.1f, kDefaultMismatchedPixels, "Minor AA diffs")},
                {"indirect.svg", Params::WithGoldenOverride(
                                     "donner/svg/renderer/testdata/golden/resvg-indirect.png")},
                {"tspan-decoration.svg",
                 Params::WithThreshold(0.1f, kDefaultMismatchedPixels, "Minor AA diffs")},
                {"underline-with-rotate-list-4.svg",
                 Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "Minor shading diffs")},

                {"indirect-with-multiple-colors.svg",
                 Params::Skip(
                     "Not impl: text-decoration full SVG2 support (line/style/color independent)")},
                {"with-textLength-on-a-single-character.svg",
                 Params::Skip(
                     "Not impl: text-decoration full SVG2 support (line/style/color independent)")},
            })),
        ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(TextTextRendering, ImageComparisonTestFixture,
                         Combine(ValuesIn(getTestsInCategory("text/text-rendering")),
                                 ValuesIn(ActiveComparisonModes())),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextTextLength, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/textLength",
                {
                    {"arabic-with-lengthAdjust.svg",
                     Params().onlyTextFull().withReason("Arabic shaping needs HarfBuzz")},
                    {"arabic.svg",
                     Params().onlyTextFull().withReason("Arabic shaping needs HarfBuzz")},
                    {"on-a-single-tspan.svg",
                     Params::Skip("Not impl: textLength + lengthAdjust attribute (text "
                                  "stretching/compressing)")},
                    {"zero.svg", Params::Skip("Not impl: textLength + lengthAdjust attribute (text "
                                              "stretching/compressing)")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextTextPath, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "text/textPath",
            {
                {"closed-path.svg", Params::WithThreshold(0.1f, 400, "Minor AA diffs")},
                {"complex.svg", Params::Skip("Deferred: vertical + circular path")},
                {"dy-with-tiny-coordinates.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-dy-with-tiny-coordinates.png",
                     0.05f)
                     .withMaxPixelsDifferent(1100)
                     .withReason(
                         "AA + minor char advance diffs, different w/ text vs. text-full so")},
                {"link-to-rect.svg", Params::Skip("Not impl: link to rect (SVG 2)")},
                {"m-A-path.svg",
                 Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "AA artifacts")},
                {"m-L-Z-path.svg", Params::WithGoldenOverride(
                                       "donner/svg/renderer/testdata/golden/resvg-m-L-Z-path.png")
                                       .withReason("Minor char")},
                {"method=stretch.svg", Params::Skip("Not impl: method=stretch")},
                {"mixed-children-1.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-mixed-children-1.png")
                     .withReason("AA diffs")},
                {"mixed-children-2.svg", Params::Skip("Bug: Kerning on textPath")},
                {"nested.svg",
                 Params::WithGoldenOverride("donner/svg/renderer/testdata/golden/resvg-nested.png")
                     .withReason("Minor char")},
                {"path-with-ClosePath.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-path-with-ClosePath.png")
                     .withReason("Minor char")},
                {"side=right.svg", Params::Skip("Not impl: side=right (SVG 2)")},
                {"simple-case.svg", Params::WithGoldenOverride(
                                        "donner/svg/renderer/testdata/golden/resvg-simple-case.png")
                                        .withReason("Minor char")},
                {"spacing=auto.svg", Params::Skip("Not impl: spacing=auto")},
                {"startOffset=-100.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-startOffset=-100.png")
                     .withReason("Minor char")},
                {"startOffset=10percent.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-startOffset=10percent.png")
                     .withReason("Minor char")},
                {"startOffset=30.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-startOffset=30.png")
                     .withReason("Minor char")},
                {"startOffset=5mm.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-startOffset=5mm.png")
                     .withReason("Minor char")},
                {"tspan-with-absolute-position.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-tspan-with-absolute-position.png")
                     .withReason("Minor char")},
                {"tspan-with-relative-position.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-tspan-with-relative-position.png")
                     .withReason("Minor char")},
                {"two-paths.svg", Params::WithGoldenOverride(
                                      "donner/svg/renderer/testdata/golden/resvg-two-paths.png")
                                      .withReason("Minor char")},
                {"very-long-text.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-very-long-text.png")
                     .withReason("AA diffs")},
                {"with-baseline-shift-and-rotate.svg",
                 Params::RenderOnly("UB: baseline-shift + rotate")},
                {"with-baseline-shift.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-with-baseline-shift.png")
                     .withReason("Minor char")},
                {"with-coordinates-on-text.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-with-coordinates-on-text.png")
                     .withReason("Minor char")},
                {"with-coordinates-on-textPath.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-with-coordinates-on-textPath.png")
                     .withReason("Minor char")},
                {"with-filter.svg", Params::Skip("Not impl: filter on textPath")},
                {"with-invalid-path-and-xlink-href.svg",
                 Params::Skip("Not impl: invalid path + href")},
                {"with-path-and-xlink-href.svg", Params::Skip("Not impl: path + xlink:href")},
                {"with-path.svg", Params::Skip("Not impl: path attr (SVG 2)")},
                {"with-rotate.svg", Params::WithGoldenOverride(
                                        "donner/svg/renderer/testdata/golden/resvg-with-rotate.png")
                                        .withReason("Minor char")},
                {"with-text-anchor.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-with-text-anchor.png")},
                {"with-transform-on-a-referenced-path.svg",
                 Params::WithGoldenOverride("donner/svg/renderer/testdata/golden/"
                                            "resvg-with-transform-on-a-referenced-path.png")
                     .withReason("Minor char")},
                {"with-transform-outside-a-referenced-path.svg",
                 Params::WithGoldenOverride("donner/svg/renderer/testdata/golden/"
                                            "resvg-with-transform-outside-a-referenced-path.png")
                     .withReason("Minor char")},
                {"with-underline.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-with-underline.png")
                     .withReason("Minor char")},
                {"writing-mode=tb.svg", Params::Skip("Deferred: writing-mode=tb on textPath")},
            })),
        ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextTref, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "text/tref",
            {
                {"link-to-a-complex-text.svg", Params::Skip("Not impl: <tref> (deprecated SVG 2)")},
                {"link-to-a-non-text-element.svg",
                 Params::Skip("Not impl: <tref> (deprecated SVG 2)")},
                {"link-to-an-external-file-element.svg",
                 Params::Skip("Not impl: <tref> (deprecated SVG 2)")},
                {"link-to-text.svg", Params::Skip("Not impl: <tref> (deprecated SVG 2)")},
                {"position-attributes.svg", Params::Skip("Not impl: <tref> (deprecated SVG 2)")},
                {"style-attributes.svg", Params::Skip("Not impl: <tref> (deprecated SVG 2)")},
                {"with-a-title-child.svg", Params::Skip("Not impl: <tref> (deprecated SVG 2)")},
                {"with-text.svg", Params::Skip("Not impl: <tref> (deprecated SVG 2)")},
                {"xml-space.svg", Params::Skip("Not impl: <tref> (deprecated SVG 2)")},
            })),
        ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextTspan, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/tspan",
                {
                    {"bidi-reordering.svg", Params::Skip("Not impl: BIDI reordering")},
                    {"nested-rotate.svg",
                     Params::Skip("Bug: Applying rotation indices across nested tspans")},
                    {"nested-whitespaces.svg", Params().withMaxPixelsDifferent(400).withReason(
                                                   "Vertical axis has different AA")},
                    {"tspan-bbox-2.svg", Params().withMaxPixelsDifferent(900).withReason(
                                             "Crosshair thin-line AA + underline uses")},
                    {"with-clip-path.svg", Params::Skip("Not impl: Interaction with `clip-path`")},
                    {"with-filter.svg", Params::Skip("Not impl: Interaction with `filter`")},
                    {"with-mask.svg", Params::Skip("Not impl: Interaction with `mask`")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextUnicodeBidi, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/unicode-bidi",
                {
                    {"bidi-override.svg",
                     Params::Skip("Not impl: unicode-bidi property (BiDi override)")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextWordSpacing, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory("text/word-spacing",
                                        {
                                            {"large-negative.svg",
                                             Params::RenderOnly("UB: word-spacing=-10000")},
                                        })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextWritingMode, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/writing-mode",
                {
                    {"arabic-with-rl.svg",
                     Params().onlyTextFull().withReason("Arabic shaping needs HarfBuzz")},
                    {"inheritance.svg", Params().withMaxPixelsDifferent(1500).withReason(
                                            "Bug: Baseline is ~2px off compared to resvg")},
                    {"japanese-with-tb.svg",
                     Params().onlyTextFull().withMaxPixelsDifferent(600).withReason(
                         "Non-ascii text, bug: y position is ~1px off")},
                    {"mixed-languages-with-tb-and-underline.svg",
                     Params::Skip("Non-ascii text, bug: underline not").onlyTextFull()},
                    {"mixed-languages-with-tb.svg",
                     Params::Skip("Non-ascii text, bug: mixed language").onlyTextFull()},
                    {"tb-and-punctuation.svg",
                     Params::Skip("Non-ascii text, bug: CJK punctuation").onlyTextFull()},
                    {"tb-rl.svg", Params().withMaxPixelsDifferent(1500).withReason(
                                      "Bug: Baseline is ~2px off compared to resvg")},
                    {"tb-with-alignment.svg", Params().withMaxPixelsDifferent(1500).withReason(
                                                  "Bug: Baseline is ~2px off compared to resvg")},
                    {"tb-with-dx-on-second-tspan.svg",
                     Params::Skip("Bug: `writing-mode=tb` with `dx`")},
                    {"tb-with-dx-on-tspan.svg", Params::Skip("Bug: `writing-mode=tb` with `dx`")},
                    {"tb-with-dy-on-second-tspan.svg",
                     Params::Skip("Bug: `writing-mode=tb` with `dy`")},
                    {"tb-with-rotate-and-underline.svg",
                     Params::RenderOnly("UB: tb with rotate and underline")},
                    {"tb-with-rotate.svg", Params::RenderOnly("UB: tb with rotate")},
                    {"tb.svg", Params().withMaxPixelsDifferent(1500).withReason(
                                   "Bug: Baseline is ~2px off compared to resvg")},

                    {"vertical-lr.svg",
                     Params::Skip("Bug: writing-mode edge cases beyond basic support")},
                    {"vertical-rl.svg",
                     Params::Skip("Bug: writing-mode edge cases beyond basic support")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

}  // namespace donner::svg
