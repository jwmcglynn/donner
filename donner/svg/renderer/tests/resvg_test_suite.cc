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

    // Canvas size matches the resvg-test-suite reference renderings. This is a
    // base param (mode-independent), so it stays on `test.params`.
    if (!test.params.canvasSize) {
      test.params.setCanvasSize(500, 500);
    }

    testPlan.emplace_back(std::move(test));
  }

  std::sort(testPlan.begin(), testPlan.end());
  return testPlan;
}

/// Params for a reference replaced by its 300x300 rendering from resvg release v0.49.0; the
/// provenance is recorded in third_party/resvg-test-suite/BUILD.bazel.
ImageComparisonParams ResvgV049Reference() {
  return ImageComparisonParams().setCanvasSize(300, 300);
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
                "filters/enable-background",
                {
                    // With enable-background ignored (SVG 2), these match the reference.
                    {"accumulate.svg",
                     Params().withReason("SVG 2 ignores enable-background; output matches")},
                    {"new-with-invalid-region-1.svg",
                     Params().withReason("SVG 2 ignores enable-background; output matches")},
                    {"new-with-invalid-region-2.svg",
                     Params().withReason("SVG 2 ignores enable-background; output matches")},
                    {"new-with-invalid-region-3.svg",
                     Params().withReason("SVG 2 ignores enable-background; output matches")},
                },
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
    Combine(
        ValuesIn(getTestsInCategory(
            "filters/feColorMatrix",
            {
                {"type=hueRotate-without-an-angle.svg",
                 Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "hueRotate(0) identity")},
                {"type=matrix-with-empty-values.svg",
                 Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "identity matrix")},
                {"type=matrix-with-non-normalized-values.svg",
                 Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "non-normalized values")},
                {"type=matrix-with-not-enough-values.svg",
                 Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "identity matrix")},
                {"type=matrix-with-too-many-values.svg",
                 Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "identity matrix")},
                {"type=matrix-without-values.svg",
                 Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "identity matrix")},
                {"type=saturate-with-a-large-coefficient.svg",
                 Params::UndefinedBehavior("saturate 99999")},
                {"type=saturate-with-negative-coefficient.svg",
                 Params::UndefinedBehavior("saturate -0.5")},
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
                    {"bias=-0.5.svg", Params::UndefinedBehavior("bias=-0.5")},
                    {"bias=0.5.svg", Params::UndefinedBehavior("bias=0.5")},
                    {"bias=9999.svg", Params::UndefinedBehavior("bias=9999")},
                    // The convolve math (window, 180° flip, target offset, divisor,
                    // preserveAlpha, linearRGB) is byte-equivalent to tiny-skia. The
                    // remaining Geode diff is a ~1px positional band per convolved
                    // feature, traced to the opacity-0.75 pattern *input* edge coverage
                    // (slug_fill 4-sample vs tiny-skia analytic), which the kernel
                    // spreads above threshold - the same root cause as the structure/svg
                    // and feImage/svg parity gaps. Not the color-space round-trip.
                    // Pixelmatch 2.0 checkerboard entries below: the new comparator
                    // exposes pre-existing semi-transparent convolve-math deltas vs
                    // resvg that white-blending masked. Each threshold is the measured
                    // minimal passing hundredth at the existing pixel allowance
                    // (PR #1285, see README_pixelmatch_2.md).
                    {"edgeMode=none.svg",
                     Params::WithThreshold(
                         0.53f, kDefaultMismatchedPixels,
                         "Checkerboard exposes convolve alpha-math delta; measured minimal")},
                    {"edgeMode=wrap.svg",
                     Params::WithThreshold(
                         0.43f, 200,
                         "Checkerboard exposes convolve edge-handling alpha delta; measured "
                         "minimal")},
                    {"order=4-2.svg",
                     Params::WithThreshold(
                         0.48f, kDefaultMismatchedPixels,
                         "Checkerboard exposes convolve alpha-math delta; measured minimal")},
                    {"order=4-4.svg",
                     Params::WithThreshold(
                         0.48f, kDefaultMismatchedPixels,
                         "Checkerboard exposes convolve alpha-math delta; measured minimal")},
                    {"order=4.svg",
                     Params::WithThreshold(
                         0.48f, kDefaultMismatchedPixels,
                         "Checkerboard exposes convolve alpha-math delta; measured minimal")},
                    {"preserveAlpha=true.svg",
                     Params::WithThreshold(
                         0.16f, kDefaultMismatchedPixels,
                         "Checkerboard exposes convolve alpha-math delta; measured minimal")},
                    {"targetX=0.svg",
                     Params::WithThreshold(
                         0.15f, kDefaultMismatchedPixels,
                         "Checkerboard exposes convolve alpha-math delta; measured minimal")},
                    {"targetX=2.svg",
                     Params::WithThreshold(
                         0.15f, kDefaultMismatchedPixels,
                         "Checkerboard exposes convolve alpha-math delta; measured minimal")},
                    {"unset-order.svg",
                     Params::WithThreshold(
                         0.15f, kDefaultMismatchedPixels,
                         "Checkerboard exposes convolve alpha-math delta; measured minimal")},
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
    Combine(
        ValuesIn(getTestsInCategory(
            "filters/feDropShadow",
            {
                {"only-stdDeviation.svg", Params::WithThreshold(0.05f, 160, "Minor blur diffs")},
                {"with-flood-color.svg", Params::WithThreshold(0.03f, 160, "Minor blur diffs")},
                // Percentage dx/dy are invalid <number> values and fall back to the default
                // offset of 2, so this case renders the same as only-stdDeviation.svg.
                {"with-percent-offset.svg", Params::WithThreshold(0.05f, 160, "Minor blur diffs")},
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
                     Params::UndefinedBehavior(
                         "implementation-defined resource limit; blur sigma is "
                         "clamped to 256 device px (kMaximumFilterPixelRadius) and "
                         "the reference clamps differently")},
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
                         "Each backend rasterizes the nested SVG at its device size and copies "
                         "it texel for texel; Geode's golden holds Geode's own rasterization of "
                         "that content and of the frame.")},
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
    Combine(ValuesIn(getTestsInCategory(
                "filters/feMorphology",
                {
                    // Pixelmatch 2.0 checkerboard exposes the transparent-vs-white
                    // background convention at eroded-shape boundaries. Each threshold
                    // is the measured minimal passing hundredth at the existing pixel
                    // allowance (PR #1285, see README_pixelmatch_2.md).
                    {"radius=0.5.svg",
                     Params::WithThreshold(
                         0.89f, kDefaultMismatchedPixels,
                         "Checkerboard exposes background convention; measured minimal")},
                    {"radius=1-10.svg",
                     Params::WithThreshold(
                         0.89f, kDefaultMismatchedPixels,
                         "Checkerboard exposes background convention; measured minimal")},
                    {"radius=10-1.svg",
                     Params::WithThreshold(
                         0.89f, kDefaultMismatchedPixels,
                         "Checkerboard exposes background convention; measured minimal")},
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
                                             Params::WithThreshold(0.11f, 120,
                                                                   "Minor shading differences")},
                                        })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFeSpecularLighting, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "filters/feSpecularLighting",
                {
                    // Pixelmatch 2.0 checkerboard exposes pre-existing
                    // specular-lighting output alpha deltas vs resvg that
                    // white-blending masked. Each threshold is the measured minimal
                    // passing hundredth at the existing pixel allowance (PR #1285, see
                    // README_pixelmatch_2.md).
                    {"with-feSpotLight-and-specular-and-exponent.svg",
                     Params::WithThreshold(
                         0.31f, kDefaultMismatchedPixels,
                         "Checkerboard exposes lighting alpha delta; measured minimal")},
                    {"with-feSpotLight-and-specularConstant=5.svg",
                     Params::WithThreshold(
                         0.63f, kDefaultMismatchedPixels,
                         "Checkerboard exposes lighting alpha delta; measured minimal")},
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
                                             Params::UndefinedBehavior("complex transform")},
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
                    {"stitchTiles=stitch.svg", Params::UndefinedBehavior("stitchTiles=stitch")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFilter, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "filters/filter",
            {
                {"invalid-FuncIRI.svg",
                 Params::Skip("Reference disagreement: PNG hides an invalid URL; Filter Effects "
                              "ignores the chain, covered by independent visible-geometry tests "
                              "(FilterReferenceStages_tests)")},
                {"negative-subregion.svg", ResvgV049Reference()},
                {"with-subregion-1.svg", ResvgV049Reference()},
                {"with-subregion-2.svg", ResvgV049Reference()},
                // Pixelmatch 2.0 checkerboard exposes pre-existing filter-region
                // compositing alpha deltas vs resvg that white-blending masked. Each
                // threshold is the measured minimal passing hundredth at the
                // existing pixel allowance (PR #1285, see README_pixelmatch_2.md).
                {"everything-via-xlink-href.svg",
                 Params::WithThreshold(
                     0.18f, kDefaultMismatchedPixels,
                     "Checkerboard exposes subregion alpha delta; measured minimal")},
                {"some-attributes-via-xlink-href.svg",
                 Params::WithThreshold(
                     0.18f, kDefaultMismatchedPixels,
                     "Checkerboard exposes subregion alpha delta; measured minimal")},
                {"with-region-and-subregion.svg",
                 Params::WithThreshold(
                     0.18f, kDefaultMismatchedPixels,
                     "Checkerboard exposes subregion alpha delta; measured minimal")},
                // Reference refreshed to resvg v0.49.0; see
                // third_party/resvg-test-suite/BUILD.bazel.
                {"complex-order-and-xlink-href.svg",
                 Params::Skip("Reference disagreement: the region's x and height cut the blur's "
                              "input at its left and bottom edges. The reference blurs with five "
                              "box passes that each drop what spreads past the region, so the "
                              "first row and column inside read alpha 84 where Donner, which "
                              "keeps that spill, reads 143, close to a single Gaussian of the "
                              "clipped input (1,275 px, all within 5 px of those "
                              "edges; Geode adds 3 frame-corner pixels); "
                              "FilterHrefRegionClipMatchesClippedSourceBlurredWithRoom pins the "
                              "region clip")
                     .setCanvasSize(300, 300)},
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
                {"transform-on-shape-with-filter-region.svg",
                 Params::Skip("Bug: We don't blur the right edge")},
                {"subregion-and-primitiveUnits=objectBoundingBox-1.svg",
                 Params::WithThreshold(0.3f, 600, "Subregion edge AA")},
                {"subregion-and-primitiveUnits=objectBoundingBox-2.svg",
                 Params::WithThreshold(0.3f, 600, "Subregion edge AA")},
                {"with-subregion-3.svg", Params::WithThreshold(0.1f, kDefaultMismatchedPixels,
                                                               "Minor shading differences")},
                {"in=BackgroundAlpha.svg",
                 Params::Skip("Not impl: Filter Effects BackgroundAlpha from the isolation-group "
                              "backdrop")},
            })),
        ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFilterFunctions, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "filters/filter-functions",
            {
                {"one-invalid-url-in-list.svg",
                 Params::Skip("Reference disagreement: PNG applies functions around a missing "
                              "URL; Filter Effects ignores the entire chain "
                              "(FilterReferenceStages.InvalidReferenceDisablesEntireChain)")},
                // Residuals are 354 simple, 370 full and 375 Geode pixels; under 4x4
                // supersampled coverage they are 302 and 297, so these PNGs are not only a
                // rasterizer generation apart. Against resvg v0.49.0's rendering the labels
                // compare at 5 (simple) and 52 (full) pixels under 4x4 coverage. A 1 px label
                // shift reaches 1508 pixels, and a 10% change to the brightness, opacity or sepia
                // amount reaches 5358 or more; the grayscale change stays under the 0.02 color
                // threshold for every pixel. The registration parser reads each entry, so the
                // identical budget is spelled out per case.
                {"color-adjust-functions-0percent.svg",
                 WithMaxPixels(390,
                               "Reference label glyphs: the PNG draws the 8-unit Noto Sans "
                               "labels about 14% heavier, with 4x4 supersampled coverage; "
                               "residual confined to the three label bands; analytic residual "
                               "375 px, supersampled 302 px; filtered squares compare exactly "
                               "(#1167, #1169)")},
                {"color-adjust-functions-100percent.svg",
                 WithMaxPixels(390,
                               "Reference label glyphs: the PNG draws the 8-unit Noto Sans "
                               "labels about 14% heavier, with 4x4 supersampled coverage; "
                               "residual confined to the three label bands; analytic residual "
                               "375 px, supersampled 302 px; filtered squares compare exactly "
                               "(#1167, #1169)")},
                {"color-adjust-functions-2.svg",
                 WithMaxPixels(390,
                               "Reference label glyphs: the PNG draws the 8-unit Noto Sans "
                               "labels about 14% heavier, with 4x4 supersampled coverage; "
                               "residual confined to the three label bands; analytic residual "
                               "375 px, supersampled 302 px; filtered squares compare exactly "
                               "(#1167, #1169)")},
                {"color-adjust-functions-200percent.svg",
                 WithMaxPixels(390,
                               "Reference label glyphs: the PNG draws the 8-unit Noto Sans "
                               "labels about 14% heavier, with 4x4 supersampled coverage; "
                               "residual confined to the three label bands; analytic residual "
                               "375 px, supersampled 302 px; filtered squares compare exactly "
                               "(#1167, #1169)")},
                {"color-adjust-functions-50percent.svg",
                 WithMaxPixels(390,
                               "Reference label glyphs: the PNG draws the 8-unit Noto Sans "
                               "labels about 14% heavier, with 4x4 supersampled coverage; "
                               "residual confined to the three label bands; analytic residual "
                               "375 px, supersampled 302 px; filtered squares compare exactly "
                               "(#1167, #1169)")},
                {"color-adjust-functions-default-value.svg",
                 WithMaxPixels(390,
                               "Reference label glyphs: the PNG draws the 8-unit Noto Sans "
                               "labels about 14% heavier, with 4x4 supersampled coverage; "
                               "residual confined to the three label bands; analytic residual "
                               "375 px, supersampled 302 px; filtered squares compare exactly "
                               "(#1167, #1169)")},
                {"color-adjust-functions-negative.svg",
                 WithMaxPixels(390,
                               "Reference label glyphs: the PNG draws the 8-unit Noto Sans "
                               "labels about 14% heavier, with 4x4 supersampled coverage; "
                               "residual confined to the three label bands; analytic residual "
                               "375 px, supersampled 302 px; filtered squares compare exactly "
                               "(#1167, #1169)")},
                {"drop-shadow-function-color-last.svg",
                 Params::Skip("Reference disagreement: the specified device offset is (10, 12.5); "
                              "the reference truncates it to (10, 12), Donner rounds to (10, 13) "
                              "(CssFilterContractShadowHalfPixelOffsetsUseRoundedPixels); Donner "
                              "at (10, 12) matches the reference within the default allowance")},
                {"drop-shadow-function-em-values.svg",
                 Params::Skip("Reference disagreement: 0.2em 0.3em at font-size 64 is 12.8 19.2 "
                              "user units, device (32, 48); the reference shadow sits at (30, 47), "
                              "which needs the user lengths truncated to 12 19 (a hypothesis the "
                              "usvg source does not show) and the scaled offset truncated as "
                              "resvg's apply_drop_shadow does (dx as i32) "
                              "(CssFilterContractEmShadowUsesComputedNondefaultFontSize)")},
                {"drop-shadow-function-mm-values.svg",
                 Params::Skip("Reference disagreement: 3mm 4mm is 11.34 15.12 user units, device "
                              "(28.35, 37.80), which Donner rounds to (28, 38); the reference "
                              "shadow sits at (27, 37), which needs the user lengths truncated to "
                              "11 15 (a hypothesis the usvg source does not show) and the scaled "
                              "offset truncated as resvg's apply_drop_shadow does (dx as i32) "
                              "(CssFilterContractShadowUnitsColorsAndDefaultMatch)")},
                {"drop-shadow-function-no-color.svg",
                 Params::Skip("Reference disagreement: the specified device offset is (10, 12.5); "
                              "the reference truncates it to (10, 12), Donner rounds to (10, 13) "
                              "(CssFilterContractShadowHalfPixelOffsetsUseRoundedPixels); Donner "
                              "at (10, 12) matches the reference within the default allowance")},
                {"drop-shadow-function.svg",
                 Params::Skip("Reference disagreement: the specified device offset is (10, 12.5); "
                              "the reference truncates it to (10, 12), Donner rounds to (10, 13) "
                              "(CssFilterContractShadowHalfPixelOffsetsUseRoundedPixels); Donner "
                              "at (10, 12) matches the reference within the default allowance")},
                {"two-exact-urls.svg",
                 Params::UndefinedBehavior(
                     "reference is the resvg UB marker image; the valid "
                     "repeated filter URL has an independent equivalent-graph "
                     "comparison (#1169)")},
                {"url-and-grayscale.svg",
                 Params::Skip("Reference disagreement: CSS grayscale must use sRGB (center "
                              "115); the historical mixed-chain reference uses linearRGB-like "
                              "center 122 (CssFilterContractMixedUrlGrayscaleUsesSrgb, #1169)")},
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
                    // The 300x300 resvg v0.49.0 rendering. The superseded reference mapped the clip
                    // through the glyph ink extents of the text rather than its glyph cells.
                    {"clip-path-with-transform-on-text.svg",
                     ResvgV049Reference().requireFeature(RendererBackendFeature::Text,
                                                         "text rendering")},
                    {"on-the-root-svg-without-size.svg",
                     Params::UndefinedBehavior("on root `<svg>` without size")},

                    {"circle-shorthand-with-stroke-box.svg",
                     Params::Skip("Harness mismatch: opaque reference background differs from "
                                  "transparent RGBA output; independent circle stroke-box "
                                  "contracts (CircleClipPath_tests)")},
                    {"circle-shorthand-with-view-box.svg",
                     Params::Skip("Harness mismatch: opaque reference background differs from "
                                  "transparent RGBA output; independent circle view-box "
                                  "contracts (CircleClipPath_tests)")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    MaskingMask, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "masking/mask",
                {
                    {"recursive-on-child.svg", Params::UndefinedBehavior("Recursive on child")},
                    // The 300x300 resvg v0.49.0 rendering, which upscales the masked image with
                    // the Mitchell cubic as Donner does.
                    {"with-image.svg", ResvgV049Reference()},
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
                     Params::WithThreshold(0.6f, 800, "Nested pattern AA (768px)")},
                    {"overflow=visible.svg", Params::UndefinedBehavior("overflow=visible")},
                    {"patternContentUnits-with-viewBox.svg",
                     Params::WithThreshold(kDefaultThreshold, 150, "Pattern AA drift")},
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
                    {"transform-and-patternTransform.svg",
                     Params{}.withGeodeGoldenOverride(
                         "donner/svg/renderer/testdata/golden/geode/"
                         "paint-servers_pattern_transform-and-patternTransform.png",
                         "Geode analytic dual-ray Slug coverage (0041 §6) on the rotated rounded "
                         "rect edge diverges from the resvg finite-sample reference by a ~1px edge "
                         "band, interacting with the pattern tile seams; the exact backend golden "
                         "keeps the case gated without a threshold exception")},
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
                                           "start/end circles: transparent per SVG 2 (#1186)")},
                {"fr=0.7.svg",
                 Params::Skip("Reference disagreement: containing start circle is transparent per "
                              "SVG 2; reference paints a reversed gradient "
                              "(ReferenceContractRadialContainingStartCircleDoesNotPaint, #1186)")},
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

INSTANTIATE_TEST_SUITE_P(
    PaintServersStop, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "paint-servers/stop",
                {
                    {"no-stop-color.svg",
                     Params::WithThreshold(0.03f, kDefaultMismatchedPixels,
                                           "OKLab sensitivity to gradient edge coverage")},
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
    Combine(ValuesIn(getTestsInCategory(
                "painting/image-rendering",
                {
                    // Uses the deprecated `optimizeSpeed` alias, which CSS Images maps to
                    // `crisp-edges`. The resvg golden chooses a different pixel-center grid under
                    // the root viewport scale; this reference uses an allowed
                    // destination-pixel-center nearest grid.
                    {"on-feImage.svg",
                     Params::WithGoldenOverride(
                         "donner/svg/renderer/testdata/golden/resvg-on-feImage.png")
                         .requireFeature(RendererBackendFeature::FilterEffects, "filter effects")
                         .withReason("Nearest sampling at destination pixel centers")},
                    // The 300x300 resvg v0.49.0 rendering; the superseded reference was flattened
                    // onto white and resolved nearest-neighbor ties toward the lower texel.
                    {"optimizeSpeed.svg",
                     ResvgV049Reference().disableBackend(
                         RendererBackend::Geode,
                         "Bug: at 3.75 device pixels per texel, every second texel boundary falls "
                         "on a pixel center; Geode's interpolated texture coordinate lands just "
                         "below it where tiny-skia's float inverse transform lands on it, so "
                         "nearest sampling picks the previous texel (564 px against resvg "
                         "v0.49.0, all inside the image; TinySkia matches)")},
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
                {"target-with-subpaths-2.svg", Params::UndefinedBehavior("Target with subpaths")},
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

INSTANTIATE_TEST_SUITE_P(
    PaintingPaintOrder, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "painting/paint-order",
            {
                // The vendored PNG loses e-x kerning at the paint-only tspan boundary. Donner's
                // custom goldens preserve continuous shaping while still gating paint order.
                {"on-tspan.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-paint-order-on-tspan.png")
                     .withReason("Preserve cross-span kerning across a paint-only tspan boundary")},
                {"on-text.svg",
                 Params{}.withGeodeGoldenOverride(
                     "donner/svg/renderer/testdata/golden/geode/"
                     "painting_paint-order_on-text.png",
                     "Geode's whole-run stroke-then-fill order is covered independently; its "
                     "analytic glyph edge coverage differs from resvg's finite-sample text "
                     "rasterization, so this exact backend golden preserves a zero-tolerance "
                     "oracle without masking paint-order behavior.")},
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
                    // Both vendored references map a stroke paint server's objectBoundingBox units
                    // through the glyph *ink* extents of "Text" (measured 42.80..156.08 x 66.96..
                    // 110.40 user units at font-size 60) instead of its glyph cells: advance width
                    // by the font's ascent and descent (42.20..157.40 x 45.66..127.38), which
                    // Donner and resvg v0.49.0 use for fill and stroke alike.
                    //
                    // A default linear gradient runs along x only, where the two boxes differ by
                    // under 2 user units, so it stays inside a loosened per-pixel tolerance. The
                    // radial gradient sees the full 1.9x height difference and cannot. Against
                    // resvg v0.49.0's 300x300 rendering, which uses the glyph cells, 358 and 364
                    // simple-text pixels (352 and 361 full text) still differ, with no alpha delta
                    // above 58 of 255 and no opaque color delta above 3. The root cause of that
                    // residual is not yet identified, so these references are not replaced.
                    {"linear-gradient-on-text.svg",
                     Params::WithThreshold(0.05f, kDefaultMismatchedPixels,
                                           "Reference uses text ink extents for stroke paint")
                         .requireFeature(RendererBackendFeature::Text, "text rendering")},
                    {"pattern-on-text.svg",
                     Params::WithThreshold(0.1f, kDefaultMismatchedPixels, "AA artifacts")},
                    {"radial-gradient-on-text.svg",
                     Params::Skip("Reference disagreement: appears to use ink extents; "
                                  "SVG2 text paint uses glyph-cell bounds")},
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
                    // The 300x300 resvg v0.49.0 rendering. The superseded reference mapped the clip
                    // through the union of the two text elements' glyph ink extents (measured
                    // 50.5..144.4 x 69.3..105.5 user units) rather than their glyph cells.
                    {"bbox-impact-3.svg", ResvgV049Reference().requireFeature(
                                              RendererBackendFeature::Text, "text rendering")},
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
                    // Raster-image references are the 300x300 renderings from resvg v0.49.0, which
                    // resamples `image-rendering: auto` with the Mitchell cubic as Donner does. The
                    // superseded references mixed several historical upscalers.
                    {"embedded-16bit-png.svg", ResvgV049Reference()},
                    {"embedded-gif.svg", ResvgV049Reference()},
                    {"embedded-jpeg-as-image-jpeg.svg", ResvgV049Reference()},
                    {"embedded-jpeg-as-image-jpg.svg", ResvgV049Reference()},
                    {"embedded-jpeg-without-mime.svg", ResvgV049Reference()},
                    {"embedded-png.svg", ResvgV049Reference()},
                    {"external-gif.svg", ResvgV049Reference()},
                    {"external-jpeg.svg", ResvgV049Reference()},
                    {"external-png.svg", ResvgV049Reference()},
                    {"image-with-float-size-scaling.svg", ResvgV049Reference()},
                    {"no-height-non-square.svg", ResvgV049Reference()},
                    {"no-height.svg", ResvgV049Reference()},
                    {"no-width-and-height.svg", ResvgV049Reference()},
                    {"no-width.svg", ResvgV049Reference()},
                    {"preserveAspectRatio=none.svg", ResvgV049Reference()},
                    {"preserveAspectRatio=xMaxYMax-meet.svg", ResvgV049Reference()},
                    {"preserveAspectRatio=xMaxYMax-slice.svg", ResvgV049Reference()},
                    {"preserveAspectRatio=xMidYMid-meet.svg", ResvgV049Reference()},
                    {"preserveAspectRatio=xMidYMid-slice.svg", ResvgV049Reference()},
                    {"preserveAspectRatio=xMinYMin-meet.svg", ResvgV049Reference()},
                    {"preserveAspectRatio=xMinYMin-slice.svg", ResvgV049Reference()},
                    {"raster-image-and-size-with-odd-numbers.svg", ResvgV049Reference()},
                    {"width-and-height-set-to-auto.svg", ResvgV049Reference()},
                    {"with-transform.svg", ResvgV049Reference()},
                    {"float-size.svg", Params::UndefinedBehavior("Float size")},
                    {"no-height-on-svg.svg", Params::UndefinedBehavior("No height")},
                    {"no-width-and-height-on-svg.svg",
                     Params::UndefinedBehavior("No width and height")},
                    {"no-width-on-svg.svg", Params::UndefinedBehavior("No width")},
                    {"embedded-svg-with-text.svg",
                     Params::Skip(
                         "Reference disagreement: the vendored reference omits the embedded SVG's "
                         "text. Donner renders it with the document font catalog; the authored "
                         "embedded-versus-direct identity test EmbeddedSvgUsesDocumentFontContext "
                         "pins that policy")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StructureStyle, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "structure/style",
                {
                    {"external-CSS.svg",
                     Params::Skip("Not impl: CSS @import loading (diagnosed and ignored)")},
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
                 Params::UndefinedBehavior("FuncIRI with invalid chars")},
                {"invalid-id-attribute-1.svg", Params::UndefinedBehavior("Invalid id attribute")},
                {"invalid-id-attribute-2.svg", Params::UndefinedBehavior("Invalid id attribute")},
                {"no-size.svg",
                 Params::Skip("API policy: caller canvas preserves user units; reference "
                              "fits content bounds without authored dimensions (#1186)")},
                {"not-UTF-8-encoding.svg",
                 Params::Skip("Unsupported input: XML byte parser requires UTF-8; transcode "
                              "and update the encoding declaration")},
                {"preserveAspectRatio-with-viewBox-not-at-zero-pos.svg",
                 Params::WithThreshold(0.13f, 500, "Viewport edge AA")},
                {"preserveAspectRatio=none.svg",
                 Params::WithThreshold(0.13f, 1000, "Viewport edge AA")},
                {"preserveAspectRatio=xMaxYMax.svg",
                 Params::WithThreshold(0.13f, 300, "Viewport edge AA")},
                {"preserveAspectRatio=xMidYMid.svg",
                 Params::WithThreshold(0.13f, 300, "Viewport edge AA")},
                {"preserveAspectRatio=xMinYMin.svg",
                 Params::WithThreshold(0.14f, kDefaultMismatchedPixels,
                                       "Has anti-aliasing artifacts.")},
                {"proportional-viewBox.svg", Params::WithThreshold(0.14f, kDefaultMismatchedPixels,
                                                                   "Has anti-aliasing artifacts.")},

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

INSTANTIATE_TEST_SUITE_P(
    StructureTransformOrigin, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "structure/transform-origin",
                {
                    {"on-image.svg", Params::WithThreshold(0.13f, 300, "Rotated edge AA")},
                    {"on-text-path.svg", Params::WithThreshold(0.13f, 300, "Rotated edge AA")},
                    {"on-text.svg", Params::WithThreshold(0.13f, 300, "Rotated edge AA")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StructureUse, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "structure/use",
                {
                    {"xlink-to-an-external-file.svg",
                     Params::Skip("Reference qualification: local fragment renders; the "
                                  "opaque reference background differs from the "
                                  "transparent SVG canvas")},
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
                    {"after-edge.svg",
                     Params::UndefinedBehavior("reference is the resvg UB marker image")},
                    {"baseline.svg",
                     Params::UndefinedBehavior("reference is the resvg UB marker image")},
                    {"ideographic.svg",
                     Params::UndefinedBehavior("reference is the resvg UB marker image")},
                    {"text-after-edge.svg",
                     Params::UndefinedBehavior("reference is the resvg UB marker image")},

                    {"hanging-on-vertical.svg",
                     Params::Skip("Reference limitation: resvg does not apply alignment-baseline "
                                  "in vertical layout; Donner follows the vertical baseline axis "
                                  "and still differs in mixed upright/sideways glyph geometry.")},
                    {"two-textPath-with-middle-on-first.svg",
                     Params().withMaxPixelsDifferent(200).withReason(
                         "Sub-pixel glyph placement along the two paths; ~108 scattered "
                         "edge pixels, no positional drift (bounding boxes match exactly)")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextBaselineShift, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/baseline-shift",
                {
                    // A 1 px text shift reaches 408 to 425 pixels; the former 0.1 color
                    // threshold let it pass at 39.
                    {"nested-with-baseline-1.svg",
                     Params().withMaxPixelsDifferent(400).withReason(
                         "Reference rasterizer generation: 4x4 supersampled coverage on the "
                         "crosshair; analytic residual 384 px, supersampled 0 px")},
                    {"nested-with-baseline-2.svg",
                     Params().withMaxPixelsDifferent(400).withReason(
                         "Reference rasterizer generation: 4x4 supersampled coverage on the "
                         "crosshair; analytic residual 384 px, supersampled 0 px")},
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
                    {"reset-size.svg",
                     Params::DeprecatedInSVG2("dominant-baseline: reset-size (SVG 1.1 value, "
                                              "removed in SVG 2); the reference is the resvg UB "
                                              "marker image")},
                    {"use-script.svg",
                     Params::DeprecatedInSVG2("dominant-baseline: use-script (SVG 1.1 value, "
                                              "removed in SVG 2); the reference is the resvg UB "
                                              "marker image")},

                    // A 1 px text shift reaches 344 pixels and passes, as it did under the former
                    // 0.1 color threshold (6 pixels); a 2 px shift (562) and the alphabetic
                    // baseline (2997) fail.
                    {"hanging.svg",
                     Params().withMaxPixelsDifferent(350).withReason(
                         "Reference rasterizer generation: 4x4 supersampled coverage on the 0.5 "
                         "unit crosshair (alpha 128/192 where Donner's analytic coverage gives "
                         "160/160); the glyph matches; analytic residual 337 px, supersampled "
                         "0 px")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextFont, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "text/font",
            {
                // The vertical crosshair is 1.25 device px wide centered on x=250. Donner's
                // analytic coverage gives the exact area split, 160/160; this PNG's 4x4
                // supersampled coverage gives 128/192. With 4x4 coverage the case compares at
                // 0 pixels, and shifting the text 1 px raises the residual to 422.
                {"font-shorthand.svg",
                 Params().withMaxPixelsDifferent(400).withReason(
                     "Reference rasterizer generation: 4x4 supersampled coverage on the 1.25 px "
                     "crosshair; the text under the shorthand matches; analytic residual 386 px, "
                     "supersampled 0 px (#1173)")},
                // `font="bold italic 64 Noto Sans"` resolves to Noto Sans Italic, the only italic
                // face in the test font set (FontSelection_tests.cc). The vendored 400x400 PNG
                // draws a synthesized bold over it with wider advances, which neither CPU text
                // tier synthesizes for document fonts, while resvg v0.49.0 rejects the unitless
                // size in the shorthand and draws 12px default serif text instead.
                {"simple-case.svg",
                 Params::Skip("Reference disagreement: the PNG synthesizes bold over Noto Sans "
                              "Italic and resvg v0.49.0 ignores the unitless shorthand; Donner "
                              "applies it and draws the Italic face (#1167, #1173, #1186)")
                     .setCanvasSize(400, 400)},
            })),
        ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextFontFamily, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "text/font-family",
            {
                // The six generic-family references (bold-sans-serif, cursive, fantasy, monospace,
                // sans-serif, serif) are the 300x300 renderings from resvg v0.49.0, which Donner
                // matches. The superseded ones were drawn in faces outside this font set:
                // sans-serif.png differed from noto-sans.png although the resvg harness maps
                // sans-serif to Noto Sans (at v0.49.0 the two are byte-identical), and the bold
                // glyph box was 181x67 px against Noto Sans Bold's 205x73 px. Which face each
                // family resolves to is asserted directly in FontSelection_tests.cc.
                {"bold-sans-serif.svg", Params().setCanvasSize(300, 300)},
                {"cursive.svg", Params().setCanvasSize(300, 300)},
                // resvg's harness resolves an all-unknown family list to its default family,
                // drawn here in Noto Serif; Donner's last-resort face is the embedded Public Sans
                // (FontSelection.UnknownFamilyFallsBackToTheEmbeddedFace).
                {"fallback-1.svg",
                 Params::Skip("Harness mismatch: resvg falls back to Noto Serif for an unknown "
                              "family, Donner to its embedded Public Sans (445 simple, 444 full "
                              "px against resvg v0.49.0) (#1167)")
                     .setCanvasSize(300, 300)},
                {"fantasy.svg", Params().setCanvasSize(300, 300)},
                {"monospace.svg", Params().setCanvasSize(300, 300)},
                {"sans-serif.svg", Params().setCanvasSize(300, 300)},
                {"serif.svg", Params().setCanvasSize(300, 300)},
            })),
        ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextFontKerning, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/font-kerning",
                {{"arabic-script.svg",
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
                {"named-value-without-a-parent.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-named-value-without-a-parent.png")
                     .withReason("Donner uses CSS Fonts Level 4, which has")},
                {"named-value.svg", Params::WithGoldenOverride(
                                        "donner/svg/renderer/testdata/golden/resvg-named-value.png")
                                        .withReason("Donner uses CSS Fonts Level 4, which has")},
                {"negative-size.svg", Params::UndefinedBehavior("negative font size")},
            })),
        ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextFontSizeAdjust, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "text/font-size-adjust",
            {{"simple-case.svg",
              Params::Skip(
                  "Reference places the adjusted text about 4 device px left of center. Both "
                  "renderings use the same adjusted size, 64 * 0.3 / (536/1000) = 35.8, and the "
                  "same glyph advances; only the anchor origin differs, and Donner's text-anchor "
                  "placement for this string and face is exact where font-size-adjust is absent "
                  "(#1173)")}})),
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
                     Params::DeprecatedInSVG2("glyph-orientation-horizontal (removed in SVG 2)")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextGlyphOrientationVertical, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/glyph-orientation-vertical",
                {{"simple-case.svg", Params::Skip("upright Latin reference comparison remains "
                                                  "unresolved after orientation and cell-metric "
                                                  "coverage")}})),
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
    Combine(ValuesIn(getTestsInCategory(
                "text/lengthAdjust",
                {
                    // Both references are resvg v0.49.0's 300x300 renderings. The superseded
                    // vertical image sat 0.28 units across the line from Donner and resvg v0.49.0,
                    // consistent with an older sideways baseline, and the superseded underline was
                    // twice the font's 0.05em underline thickness.
                    {"vertical.svg", Params().setCanvasSize(300, 300)},
                    {"with-underline.svg", Params().setCanvasSize(300, 300)},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextLetterSpacing, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/letter-spacing",
                {
                    {"large-negative.svg", Params::UndefinedBehavior("negative letter-spacing")},
                    {"mixed-scripts.svg", Params().onlyTextFull().withReason("Arabic shaping")},
                    // resvg v0.49.0 quotes the family as 'Mplus 1p'; unquoted it is invalid
                    // and the snapshot's reference was drawn in a fallback face.
                    {"non-ASCII-character.svg", Params().setCanvasSize(300, 300)},
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
                    {"bidi-reordering.svg", Params().onlyTextFull().withReason("Arabic shaping")},
                    {"complex-grapheme-split-by-tspan.svg",
                     Params::UndefinedBehavior("grapheme split by tspan")},
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
                         .onlyTextFull()
                         .withReason(
                             "Codepoint positioning reference preserves complete emoji clusters")},
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
                    // The reference is resvg v0.49.0's 300x300 rendering, which draws all three
                    // lines in Mplus 1p, the only face in the test font set covering these CJK
                    // codepoints. The superseded one used per-language faces outside the set.
                    {"xml-lang=ja.svg", Params().setCanvasSize(300, 300)},
                    {"zalgo.svg",
                     Params().withMaxPixelsDifferent(265).onlyTextFull().withReason(
                         "Reference rasterizer generation: 4x4 supersampled coverage on the "
                         "crosshair and diacritic edges; analytic residual 257 px, supersampled "
                         "1 px; a 1 px text shift reaches 468")},
                    // These three differ from their PNGs only in edge coverage: the crosshair
                    // and glyph edges compare at 0 pixels under 4x4 supersampled coverage, and
                    // a 1 px text shift raises each residual past its budget (507, 435 and
                    // 441 pixels). The simple tier lacks the mixed-font ligature shaping.
                    {"ligatures-handling-in-mixed-fonts-1.svg",
                     Params().withMaxPixelsDifferent(415).onlyTextFull().withReason(
                         "Reference rasterizer generation: 4x4 supersampled coverage on the "
                         "crosshair and glyph edges; analytic residual 399 px, supersampled 0 px; "
                         "simple tier: Bug #1176, mixed-font ligature shaping")},
                    {"ligatures-handling-in-mixed-fonts-2.svg",
                     Params().withMaxPixelsDifferent(355).onlyTextFull().withReason(
                         "Reference rasterizer generation: 4x4 supersampled coverage on the "
                         "crosshair and glyph edges; analytic residual 343 px, supersampled 0 px; "
                         "simple tier: Bug #1176, mixed-font ligature shaping")},
                    {"real-text-height.svg",
                     Params().withMaxPixelsDifferent(410).withReason(
                         "Reference rasterizer generation: 4x4 supersampled coverage on the "
                         "crosshair and glyph edges; analytic residual 397 px, supersampled 0 px")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextTextAnchor, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/text-anchor",
                {
                    // Pixelmatch 2.0 checkerboard exposes the transparent-vs-white
                    // background convention (Donner renders no background; the
                    // reference is opaque). The threshold is the measured minimal
                    // passing hundredth at the existing pixel allowance (PR #1285, see
                    // README_pixelmatch_2.md).
                    {"coordinates-list.svg",
                     Params::WithThreshold(
                         0.89f, kDefaultMismatchedPixels,
                         "Checkerboard exposes background convention; measured minimal")},
                    {"on-tspan-with-arabic.svg",
                     Params().requireFeature(RendererBackendFeature::TextFull)},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextTextDecoration, ImageComparisonTestFixture,
    Combine(ValuesIn(getTestsInCategory(
                "text/text-decoration",
                {
                    // 0.12 covers both CPU text configurations: simple text counts 100
                    // at 0.11 while full text counts 103, and all three cases count 68
                    // at 0.12 (PR #1285).
                    {"all-types-inline-comma-separated.svg",
                     Params::WithThreshold(0.12f, kDefaultMismatchedPixels, "Minor AA diffs")},
                    {"all-types-inline-no-spaces.svg",
                     Params::WithThreshold(0.12f, kDefaultMismatchedPixels, "Minor AA diffs")},
                    {"all-types-inline.svg",
                     Params::WithThreshold(0.12f, kDefaultMismatchedPixels, "Minor AA diffs")},
                    {"indirect.svg", Params::WithGoldenOverride(
                                         "donner/svg/renderer/testdata/golden/resvg-indirect.png")},
                    {"tspan-decoration.svg",
                     Params::WithThreshold(0.1f, kDefaultMismatchedPixels, "Minor AA diffs")},
                    {"underline-with-rotate-list-4.svg",
                     Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "Minor shading diffs")},

                    // Each decoration takes the paint of the element that declared it (SVG 2
                    // section 11.11). Without per-origin paint the line-through takes the span's
                    // green and the case differs by 1589 px; a 1 px text shift reaches 2270 px.
                    {"indirect-with-multiple-colors.svg",
                     Params().withMaxPixelsDifferent(395).withReason(
                         "Reference rasterizer generation: 4x4 supersampled coverage on the x=100 "
                         "crosshair; analytic residual 382 px, supersampled 2 px")},
                    // The reference is resvg v0.49.0's 300x300 rendering; the superseded underline
                    // was twice the font's 0.05em underline thickness.
                    {"with-textLength-on-a-single-character.svg", Params().setCanvasSize(300, 300)},
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
                    {"zero.svg",
                     Params().withMaxPixelsDifferent(410).withReason(
                         "SVG2 anchors compressed pen extents after textLength adjustment; "
                         "the reference is an independent resvg 0.49.0 render with the bundled "
                         "Noto Sans face at the original 500x500 size. The existing rasterizer "
                         "allowance is retained.")},
                })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextTextPath, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "text/textPath",
            {
                // Pixelmatch 2.0 checkerboard exposes the transparent-vs-white
                // background convention (Donner renders no background; the
                // reference is opaque). Each threshold is the measured minimal
                // passing hundredth at the existing pixel allowance (PR #1285, see
                // README_pixelmatch_2.md).
                {"link-to-rect.svg",
                 Params::WithThreshold(
                     0.89f, kDefaultMismatchedPixels,
                     "Checkerboard exposes background convention; measured minimal")},
                {"with-path-and-xlink-href.svg",
                 Params::WithThreshold(
                     0.89f, kDefaultMismatchedPixels,
                     "Checkerboard exposes background convention; measured minimal")},
                {"with-path.svg",
                 Params::WithThreshold(
                     0.89f, kDefaultMismatchedPixels,
                     "Checkerboard exposes background convention; measured minimal")},
                {"closed-path.svg", Params::WithThreshold(0.1f, 400, "Minor AA diffs")},
                {"complex.svg", Params::Skip("Deferred: vertical + circular path")},
                {"dy-with-tiny-coordinates.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-dy-with-tiny-coordinates.png")
                     .withMaxPixelsDifferent(1100)
                     .withReason(
                         "resvg 0.47.0 reference; full-text small-font placement residual")},
                {"m-A-path.svg",
                 Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "AA artifacts")},
                {"m-L-Z-path.svg", Params::WithGoldenOverride(
                                       "donner/svg/renderer/testdata/golden/resvg-m-L-Z-path.png")
                                       .withReason("Minor char")},
                {"method=stretch.svg",
                 Params::Skip("Implemented by warping outline endpoints and control points along "
                              "path normals; the vendored reference is byte-identical to "
                              "simple-case.png and therefore pins align placement. Independent "
                              "geometry and renderer regressions cover the stretch contract")},
                {"mixed-children-1.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-mixed-children-1.png")
                     .withReason("SVG2 endpoint continuation and anti-aliasing differences")},
                {"mixed-children-2.svg",
                 Params::Skip("SVG 2 positions text after textPath at the geometric path endpoint; "
                              "the reference continues from the occupied text extent")},
                {"nested.svg",
                 Params::WithGoldenOverride("donner/svg/renderer/testdata/golden/resvg-nested.png")
                     .withReason("Minor char")},
                {"path-with-ClosePath.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-path-with-ClosePath.png")
                     .withReason("Minor char")},
                {"side=right.svg",
                 Params::WithThreshold(0.05f, kDefaultMismatchedPixels,
                                       "Half-turned glyph edges: 3427 of 3430 raw differences are "
                                       "boundary texels, centroid within 0.1px, ink within 0.03%")},
                {"simple-case.svg", Params::WithGoldenOverride(
                                        "donner/svg/renderer/testdata/golden/resvg-simple-case.png")
                                        .withReason("Minor char")},
                {"spacing=auto.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-simple-case.png")
                     .withReason("spacing=auto lets the user agent choose, and Donner chooses the "
                                 "same spacing as exact, so this draws the same picture as "
                                 "simple-case.svg and shares its reference; its own vendored PNG "
                                 "is byte-identical to the stale simple-case.png")},
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
                     .withReason("Reference generated independently with resvg 0.47.0")},
                {"two-paths.svg", Params::WithGoldenOverride(
                                      "donner/svg/renderer/testdata/golden/resvg-two-paths.png")
                                      .withReason("Minor char")},
                {"very-long-text.svg",
                 Params::WithGoldenOverride(
                     "donner/svg/renderer/testdata/golden/resvg-very-long-text.png")
                     .withReason("SVG2 endpoint continuation and anti-aliasing differences")},
                {"with-baseline-shift-and-rotate.svg",
                 Params::UndefinedBehavior("baseline-shift + rotate")},
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
                // The filter itself is applied to the textPath now, but Donner's glyph coverage
                // is about 5% lighter than the reference's (measured on the unfiltered
                // text/tspan/with-clip-path case: 1140473 versus 1208567 total alpha over the
                // glyphs). The unfiltered comparisons pass because pixelmatch excludes
                // anti-aliased edge pixels, while a Gaussian blur spreads the missing coverage
                // into the glyph interiors, where it is counted.
                {"with-filter.svg", Params::Skip("Glyph coverage is lighter than the reference")},
                {"with-invalid-path-and-xlink-href.svg",
                 Params::Skip("Reference disagrees with the corpus: after invalid `path` is "
                              "ignored, relative `xlink:href=\"path1\"` names an external "
                              "document, not the local element. Without an admitted external SVG "
                              "resource it is unresolved; independent control-scene coverage pins "
                              "that policy")},
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
                {"writing-mode=tb.svg",
                 Params::Skip("Vertical path frames are implemented; upright CJK glyph origins "
                              "still differ between the simple/full backends and the reference")},
            })),
        ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextTref, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "text/tref",
            {
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
    Combine(
        ValuesIn(getTestsInCategory(
            "text/tspan",
            {
                {"bidi-reordering.svg", Params().onlyTextFull().withReason("Arabic shaping")},
                {"nested-whitespaces.svg",
                 Params().withMaxPixelsDifferent(400).withReason(
                     "Reference rasterizer generation: 4x4 supersampled coverage on the "
                     "crosshair; analytic residual 387 px, supersampled 0 px; a 1 px text shift "
                     "reaches 416")},
                {"tspan-bbox-2.svg", Params().withMaxPixelsDifferent(900).withReason(
                                         "Crosshair thin-line AA + underline uses")},
                // Same cause as text/textPath/with-filter.svg: the span filter applies, but
                // Donner's glyph coverage is about 5% lighter than the reference's, which only
                // a blur makes visible away from the glyph edges.
                {"with-filter.svg", Params::Skip("Glyph coverage is lighter than the reference")},
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
                                             Params::UndefinedBehavior("word-spacing=-10000")},
                                        })),
            ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextWritingMode, ImageComparisonTestFixture,
    Combine(
        ValuesIn(getTestsInCategory(
            "text/writing-mode",
            {
                {"arabic-with-rl.svg",
                 Params().onlyTextFull().withReason("Arabic shaping needs HarfBuzz")},
                // The 300x300 entries use references refreshed from upstream resvg, which renders
                // them at that size (provenance in third_party/resvg-test-suite/BUILD.bazel).
                {"inheritance.svg", Params().setCanvasSize(300, 300)},
                {"japanese-with-tb.svg",
                 Params::Skip(
                     "Reference/spec disagreement: available vmtx origin is 860; historical PNG "
                     "places upright glyphs lower. Covered by TextBackendFullVerticalOrigin."
                     "SvgUprightPlacementUsesAvailableVerticalOrigins")
                     .onlyTextFull()},
                {"mixed-languages-with-tb-and-underline.svg",
                 Params::Skip("Bug #1172: per-cluster fallback draws every glyph, but without "
                              "bidi runs the Arabic word shares the left-to-right Amiri run with "
                              "`Hi!` and is shaped unjoined in logical order, and the vertical "
                              "underline is not drawn (3180 px)")
                     .onlyTextFull()},
                {"mixed-languages-with-tb.svg", Params().setCanvasSize(300, 300).onlyTextFull()},
                {"tb-and-punctuation.svg",
                 Params::Skip("#1172: punctuation forms and spacing match; upright glyphs sit "
                              "0.026em lower (vhea ascender origin vs resvg centering on "
                              "hhea ascent/descent), 371 px")
                     .setCanvasSize(300, 300)
                     .onlyTextFull()},
                {"tb-rl.svg", Params().setCanvasSize(300, 300)},
                {"tb-with-alignment.svg", Params().setCanvasSize(300, 300)},
                {"tb-with-dx-on-second-tspan.svg", Params().setCanvasSize(300, 300)},
                {"tb-with-dx-on-tspan.svg", Params().setCanvasSize(300, 300)},
                {"tb-with-dy-on-second-tspan.svg", Params().setCanvasSize(300, 300)},
                {"tb-with-rotate-and-underline.svg",
                 Params::UndefinedBehavior("tb with rotate and underline")},
                {"tb-with-rotate.svg", Params::UndefinedBehavior("tb with rotate")},
                {"tb.svg", Params().setCanvasSize(300, 300)},
            })),
        ValuesIn(ActiveComparisonModes())),
    TestNameFromFilename);

}  // namespace donner::svg
