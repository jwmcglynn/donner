#include <gmock/gmock.h>

#include <functional>
#include <optional>

#include "donner/base/tests/Runfiles.h"
#include "donner/svg/renderer/tests/ImageComparisonTestFixture.h"

using testing::ValuesIn;

namespace donner::svg {

using Params = ImageComparisonParams;

namespace {

/// True when the active test binary is the Geode variant
/// (`resvg_test_suite_geode_*`). Used by the category and filename
/// auto-gates to decide whether to widen thresholds or apply feature
/// gates. The Skia / TinySkia variants have their own binaries, so
/// this is a compile-constant per binary — checking it at gate
/// evaluation time is safe at static-init time because
/// `ActiveRendererBackend()` is backed by a per-backend translation
/// unit (see RendererTestBackendGeode.cc etc).
const bool kActiveIsGeode = ActiveRendererBackend() == RendererBackend::Geode;

/// Widen the per-pixel threshold on Geode only, preserving the
/// existing tiny-skia / Skia strictness. Geode's 4× MSAA rasterizer
/// quantises edge coverage to 5 distinct alpha values (0, 64, 128,
/// 191, 255); tiny-skia's 16× supersample produces 17. The maximum
/// per-pixel alpha drift is therefore 1/16 ≈ 6.25%, which trips the
/// default 2% threshold on anti-aliased edges even though the shape
/// geometry is identical. Tests dominated by thin anti-aliased strokes
/// — e.g. nested `<image>` viewport frames in structure/image — need
/// a ~10% threshold to absorb this quantisation without inflating
/// `maxMismatchedPixels`.
void widenThresholdForGeode(ImageComparisonParams& p, float threshold = 0.3f) {
  if (kActiveIsGeode) {
    if (threshold > p.threshold) {
      p.threshold = threshold;
    }
  }
}

/// Category-level auto-gate for the Geode backend. Geode is only
/// feature-complete for fills/strokes/gradients/patterns/images/basic
/// shapes today — filters (Phase 7), text (Phase 4), clipping/masking
/// (Phase 3), markers (Phase 6), mix-blend-mode / isolation (Phase 9)
/// all still need to land before the matching resvg categories can
/// run under Geode.
///
/// This helper returns an `ImageComparisonParams` builder that, when
/// merged onto a testcase, cleanly skips it on Geode while leaving
/// Skia/TinySkia unaffected. The merge preserves any explicit Skip or
/// threshold override from the per-test overrides map so that we
/// don't over-widen on the CPU backends.
///
/// Returns an empty optional when the category has no Geode-specific
/// gate. The string_view inside the returned params has static
/// lifetime (string literal) so it can outlive the call.
std::optional<std::function<void(ImageComparisonParams&)>>
geodeCategoryGate(std::string_view category) {
  // Filters: whole `filters/` tree (feBlend, feColorMatrix, feComposite,
  // feDiffuseLighting, feDropShadow, feImage, feTurbulence, filter,
  // filter-functions, flood-color, flood-opacity, enable-background, …)
  if (category.rfind("filters/", 0) == 0 || category == "filters") {
    return [](ImageComparisonParams& p) {
      p.requireFeature(RendererBackendFeature::FilterEffects,
                       "filter effects (Geode Phase 7)");
    };
  }

  // Text: whole `text/` tree plus the standalone shape-with-text cases.
  if (category.rfind("text/", 0) == 0 || category == "text") {
    return [](ImageComparisonParams& p) {
      p.requireFeature(RendererBackendFeature::Text, "text rendering (Geode Phase 4)");
    };
  }

  // Clip-paths (`masking/clip`, `masking/clipPath`, `masking/clip-rule`)
  // run through the Phase 3b mask pipeline, and `<mask>` elements run
  // through the Phase 3c mask-blit pipeline — no wholesale category
  // gate here. Individual per-file overrides handle any remaining
  // divergences.

  // Markers: Phase 6.
  if (category == "painting/marker") {
    return [](ImageComparisonParams& p) {
      p.disableBackend(RendererBackend::Geode, "markers (Geode Phase 6)");
    };
  }

  // Mix-blend-mode and isolation: Phase 9 (compositing).
  if (category == "painting/mix-blend-mode" || category == "painting/isolation") {
    return [](ImageComparisonParams& p) {
      p.disableBackend(RendererBackend::Geode, "mix-blend-mode / isolation (Geode Phase 9)");
    };
  }

  return std::nullopt;
}

/// Per-file auto-gate for the Geode backend. Many resvg tests live in
/// non-text categories but still embed text/tspan, markers, or
/// clip-path references — for example `painting/fill/on-text.svg` sits
/// under `painting/fill` but can only render once text lands. Gating
/// them by filename substring keeps the auto-gate close to the test
/// file without forcing us to hand-maintain a per-test override map
/// across every category.
///
/// `category` is the directory-relative category path
/// (e.g. `"structure/image"`) — used by rules that only apply inside
/// one category tree, like the nested-`<image>` preserveAspectRatio
/// threshold widening.
///
/// Returns a mutator that applies the right skip/feature bit, or
/// nullopt if no cross-category gate matches.
std::optional<std::function<void(ImageComparisonParams&)>>
geodeFilenameGate(std::string_view category, std::string_view filename) {
  const auto contains = [&](std::string_view needle) {
    return filename.find(needle) != std::string_view::npos;
  };

  // Text-in-non-text-category: text children of fill/stroke/opacity/
  // visibility/display/pattern/<a> tests.
  if (contains("on-text") || contains("on-tspan") || contains("text-child") ||
      contains("with-text") || contains("with-a-text") ||
      contains("optimizeSpeed-on-text")) {
    return [](ImageComparisonParams& p) {
      p.requireFeature(RendererBackendFeature::Text, "text rendering (Geode Phase 4)");
    };
  }

  // Marker-in-non-marker-category: overflow/shape-rendering tests
  // that use <marker>.
  if (contains("on-marker") || contains("path-with-marker") ||
      contains("with-marker")) {
    return [](ImageComparisonParams& p) {
      p.disableBackend(RendererBackend::Geode, "markers (Geode Phase 6)");
    };
  }

  // Clip-path-in-non-clip-category: display=none and systemLanguage
  // tests that use <clipPath>. Also the `bBox-impact` / `bbox-impact`
  // tests in painting/{display,opacity,visibility} which verify that
  // invisible shapes don't affect bounding boxes — their reference
  // rendering uses a clipPath circle to isolate the bbox region.
  if (contains("on-clipPath") || contains("with-clip-path") ||
      contains("bBox-impact") || contains("bbox-impact")) {
    return [](ImageComparisonParams& p) {
      p.disableBackend(RendererBackend::Geode, "clipping/masking (Geode Phase 3)");
    };
  }

  // `structure/image/preserveAspectRatio=xMaxYMax-slice-on-svg` is
  // currently 104 pixels past the default 100-px max even at the
  // widened 0.3 per-pixel threshold. The failing pixels form a thin
  // fringe along the curved green stroke inside the embedded data-URL
  // SVG: 4× MSAA places the sub-pixel stroke edge on a different side
  // of the pixel boundary than tiny-skia's 16× supersample on roughly
  // 4% of the edge pixels, and those are 100% diffs (fully-coloured
  // stroke vs background), not something a threshold bump can absorb.
  // Historically this test was disabled under the mistaken "path
  // clipping (Phase 3)" rationale; Phase 3a polygon clipping confirms
  // no non-axis-aligned transform is in play (ancestor transform is
  // `matrix(16 0 0 16 10 175)`), so polygon clipping isn't the lever
  // to close this gap. Leaving disabled as an AA-quantisation TODO
  // until Geode picks up a finer sample pattern or analytic stroke AA.
  // TODO(geode): upgrade to 8× / 16× MSAA or analytic stroke AA to
  // shed the thin-stroke fringe pixels on nested-image tests.
  if (category == "structure/image" &&
      filename == "preserveAspectRatio=xMaxYMax-slice-on-svg.svg") {
    return [](ImageComparisonParams& p) {
      p.disableBackend(RendererBackend::Geode,
                       "4× MSAA thin-stroke fringe on nested image data URL");
    };
  }

  // `structure/image/preserveAspectRatio=*` nested `<image>` element tests.
  // Geode's 4× MSAA differs from tiny-skia's 16× supersample by up to
  // 6.25% per-pixel alpha on anti-aliased edges; the thin stroked
  // frames in these tests trip the default 2% threshold. Widen on
  // Geode only. The `structure/svg/preserveAspectRatio=*` sibling set
  // already passes at the default threshold because those tests
  // render a larger stroked frame at identical AA quantisation.
  if (category == "structure/image" && contains("preserveAspectRatio")) {
    return [](ImageComparisonParams& p) { widenThresholdForGeode(p); };
  }

  // `shapes/line/no-x1-coordinate`, `painting/stroke/control-points-clamping-1`,
  // and the last remaining `structure/image/preserveAspectRatio_xMaxYMax_slice_on_svg`
  // all finish within 2–6 pixels of the 100 max — the diff is the same
  // 4× MSAA quantisation as the preserveAspectRatio cluster but on a
  // different category path. Per-file widening.
  if (filename == "no-x1-coordinate.svg" ||
      filename == "control-points-clamping-1.svg" ||
      filename == "preserveAspectRatio=xMaxYMax-slice-on-svg.svg") {
    return [](ImageComparisonParams& p) { widenThresholdForGeode(p); };
  }

  // `painting/stroke-linejoin/miter` renders 6 stroked polylines with
  // different `stroke-miterlimit` values. On Geode, the sharp interior
  // joins where miter-limit falls back to bevel produce ~180 Y-delta
  // pixels at the tip of the bevel cut vs tiny-skia's reference.
  // Visual inspection shows the bevel IS applied but its corner is in
  // a slightly different pixel than tiny-skia — rendering a 1-2 pixel
  // triangle of stroke in the wrong spot. Not an AA drift; it's a
  // geometric offset in the miter-to-bevel fallback path of
  // `Path::strokeToFill`.
  // TODO(geode): align the bevel-fallback corner computation in
  // `emitJoin`'s outside-turn branch with tiny-skia's reference.
  if (category == "painting/stroke-linejoin" && filename == "miter.svg") {
    return [](ImageComparisonParams& p) {
      p.disableBackend(RendererBackend::Geode,
                       "bevel-fallback corner geometry differs from tiny-skia");
    };
  }

  // `paint-servers/pattern/tiny-pattern-upscaled` renders a 2×2 tile
  // pattern scaled 10× containing a circle, tiled across a
  // rounded-rect fill. Geode samples the pre-rendered tile via
  // bilinear sampling at device pixels; tiny-skia rasterizes the
  // pattern tile directly in user-space coordinates without going
  // through an intermediate texture. The intermediate texture
  // introduces ~3.5k pixels of sub-pixel-position drift at every
  // circle edge inside the tile, multiplied across ~64 visible
  // circles. The fix is to sample the pattern in user-space (without
  // the intermediate tile texture) for the small-tile upscaled case.
  // TODO(geode): sample the pattern directly in user-space for small
  // tiles where the tile texture adds more error than it saves.
  if (category == "paint-servers/pattern" && filename == "tiny-pattern-upscaled.svg") {
    return [](ImageComparisonParams& p) {
      p.disableBackend(RendererBackend::Geode,
                       "small-tile pattern texture introduces sub-pixel drift");
    };
  }


  return std::nullopt;
}

// Discover every .svg test in one category directory under the resvg-test-suite
// tree. Example: getTestsInCategory("painting/fill") → all .svg files in
// <runfiles>/resvg-test-suite/tests/painting/fill/.
//
// Overrides is keyed by the bare filename (e.g. "rgb-int-int-int.svg") and
// picks per-test params (Skip, threshold, golden override, etc). Any file not
// in the overrides map uses defaultParams. When the category matches a
// Geode-blocked feature (filters, text, clipping, markers, mix-blend-mode,
// isolation), every resulting testcase is also tagged with the matching
// `requireFeature` / `disableBackend` bit so the Geode backend skips cleanly
// while Skia/TinySkia still run the existing coverage.
std::vector<ImageComparisonTestcase> getTestsInCategory(
    std::string_view category,
    std::map<std::string, ImageComparisonParams> overrides = {},
    ImageComparisonParams defaultParams = {}) {
  const std::string kTestsRoot =
      Runfiles::instance().RlocationExternal("resvg-test-suite", "tests");
  const std::filesystem::path kCategoryDir =
      std::filesystem::path(kTestsRoot) / category;

  std::vector<ImageComparisonTestcase> testPlan;
  if (!std::filesystem::exists(kCategoryDir)) {
    return testPlan;
  }

  const auto geodeGate = geodeCategoryGate(category);

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

    // Apply the Geode category gate on top of whatever per-test or
    // per-category defaults landed above. `requireFeature` /
    // `disableBackend` are additive: explicit Skip tests stay skipped,
    // threshold overrides keep their thresholds on CPU backends, and
    // Geode ends up with the feature bit that makes it skip cleanly.
    if (geodeGate && !test.params.skip) {
      (*geodeGate)(test.params);
    }

    // Per-file cross-category gate: catches text / marker / clip-path
    // tests that live in non-text/non-marker/non-clip directories but
    // still need the corresponding feature to render.
    if (!test.params.skip) {
      if (auto filenameGate = geodeFilenameGate(category, filename); filenameGate) {
        (*filenameGate)(test.params);
      }
    }

    // Canvas size matches the resvg-test-suite reference renderings.
    test.params.setCanvasSize(500, 500);

    testPlan.emplace_back(std::move(test));
  }

  std::sort(testPlan.begin(), testPlan.end());
  return testPlan;
}

}  // namespace

TEST_P(ImageComparisonTestFixture, ResvgTest) {
  const ImageComparisonTestcase& testcase = GetParam();

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
                                 Runfiles::instance().RlocationExternal("resvg-test-suite", ""));
  renderAndCompare(document, testcase.svgFilename, goldenFilename.string().c_str());
}

INSTANTIATE_TEST_SUITE_P(
    FiltersEnableBackground, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory(
        "filters/enable-background",
        {
            // Not impl: `enable-background` attribute + `in=BackgroundImage`/
            // `in=BackgroundAlpha` filter inputs. Both were deprecated in
            // SVG 2 and replaced by `<filter>` element chains and CSS
            // `backdrop-filter`. Donner has never implemented them; the
            // existing filter suite already skips the legacy test cases
            // with the same rationale (see `in=BackgroundAlpha` / `=BackgroundImage`
            // skips elsewhere in this file). See
            // docs/unsupported_svg1_features.md.
            {"accumulate-with-new.svg", Params::Skip("Not impl: `enable-background` (deprecated SVG 1.1)")},
            {"filter-on-shape.svg", Params::Skip("Not impl: `enable-background` (deprecated SVG 1.1)")},
            {"inherit.svg", Params::Skip("Not impl: `enable-background` (deprecated SVG 1.1)")},
            {"new-with-region.svg", Params::Skip("Not impl: `enable-background` (deprecated SVG 1.1)")},
            {"new.svg", Params::Skip("Not impl: `enable-background` (deprecated SVG 1.1)")},
            {"shapes-after-filter.svg", Params::Skip("Not impl: `enable-background` (deprecated SVG 1.1)")},
            {"stop-on-the-first-new-1.svg", Params::Skip("Not impl: `enable-background` (deprecated SVG 1.1)")},
            {"stop-on-the-first-new-2.svg", Params::Skip("Not impl: `enable-background` (deprecated SVG 1.1)")},
            {"with-clip-path.svg", Params::Skip("Not impl: `enable-background` (deprecated SVG 1.1)")},
            {"with-filter-on-the-same-element.svg", Params::Skip("Not impl: `enable-background` (deprecated SVG 1.1)")},
            {"with-filter.svg", Params::Skip("Not impl: `enable-background` (deprecated SVG 1.1)")},
            {"with-mask.svg", Params::Skip("Not impl: `enable-background` (deprecated SVG 1.1)")},
            {"with-opacity-1.svg", Params::Skip("Not impl: `enable-background` (deprecated SVG 1.1)")},
            {"with-opacity-2.svg", Params::Skip("Not impl: `enable-background` (deprecated SVG 1.1)")},
            {"with-opacity-3.svg", Params::Skip("Not impl: `enable-background` (deprecated SVG 1.1)")},
            {"with-opacity-4.svg", Params::Skip("Not impl: `enable-background` (deprecated SVG 1.1)")},
            {"with-transform.svg", Params::Skip("Not impl: `enable-background` (deprecated SVG 1.1)")},
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(FiltersFeBlend, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("filters/feBlend")),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFeColorMatrix, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("filters/feColorMatrix",
                                {
                                    {"type=hueRotate-without-an-angle.svg", Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "hueRotate(0) identity")},
                                    {"type=hueRotate.svg", Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "hueRotate(30)")},
                                    {"type=matrix-with-empty-values.svg", Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "identity matrix")},
                                    {"type=matrix-with-non-normalized-values.svg", Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "non-normalized values")},
                                    {"type=matrix-with-not-enough-values.svg", Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "identity matrix")},
                                    {"type=matrix-with-too-many-values.svg", Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "identity matrix")},
                                    {"type=matrix-without-values.svg", Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "identity matrix")},
                                    {"type=matrix.svg", Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "type=matrix")},
                                    {"type=saturate-with-a-large-coefficient.svg", Params::RenderOnly("saturate 99999 (UB)")},
                                    {"type=saturate-with-negative-coefficient.svg", Params::RenderOnly("saturate -0.5 (UB)")},
                                    {"type=saturate-without-a-coefficient.svg", Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "identity saturate")},
                                    {"type=saturate.svg", Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "saturate")},
                                    {"without-attributes.svg", Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "no attrs identity")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(FiltersFeComponentTransfer, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("filters/feComponentTransfer")),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(FiltersFeComposite, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("filters/feComposite")),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFeConvolveMatrix, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("filters/feConvolveMatrix",
                                {
                                    {"bias=-0.5.svg", Params::RenderOnly("UB: bias=-0.5")},
                                    {"bias=0.5.svg", Params::RenderOnly("UB: bias=0.5")},
                                    {"bias=9999.svg", Params::RenderOnly("UB: bias=9999")},
                                    {"edgeMode=wrap-with-matrix-larger-than-target.svg", Params::RenderOnly("UB: wrap with oversized kernel")},
                                    {"edgeMode=wrap.svg", Params::WithThreshold(kDefaultThreshold, 200, "Minor algorithm differences on edge handling (180px)")},
                                    {"kernelMatrix-with-zero-sum-and-no-divisor.svg", Params::RenderOnly("Skia MatrixConvolution edge shift vs tiny-skia")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFeDiffuseLighting, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("filters/feDiffuseLighting",
                                {
                                    {"complex-transform.svg", Params::WithThreshold(0.2f, kDefaultMismatchedPixels, "Shading differences, donner is smoother")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(FiltersFeDisplacementMap, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("filters/feDisplacementMap")),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(FiltersFeDistantLight, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("filters/feDistantLight")),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFeDropShadow, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("filters/feDropShadow",
                                {
                                    {"only-stdDeviation.svg", Params::WithThreshold(0.04f, kDefaultMismatchedPixels, "Minor blur diffs")},
                                    {"with-flood-color.svg", Params::WithThreshold(0.03f, kDefaultMismatchedPixels, "Minor blur diffs")},
                                
                                    {"with-percent-offset.svg", Params::Skip("Bug: feDropShadow edge case")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(FiltersFeFlood, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("filters/feFlood")),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFeGaussianBlur, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("filters/feGaussianBlur",
                                {
                                    {"complex-transform.svg", Params::WithThreshold(0.03f, kDefaultMismatchedPixels, "Minor AA differences")},
                                    {"huge-stdDeviation.svg", Params::RenderOnly("Extreme sigma=1000; output is implementation-defined")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFeImage, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("filters/feImage",
                                {
                                    {"chained-feImage.svg", Params::WithThreshold(kDefaultThreshold, 22000, "Chained feImage fragment refs")},
                                    {"embedded-png.svg", Params::WithThreshold(0.05f, 100, "Bilinear interpolation + sRGB↔linear roundtrip")},
                                    {"empty.svg", Params::Skip("Linux CI: std::bad_alloc in test setup.")},
                                    {"link-on-an-element-with-complex-transform.svg", Params::WithThreshold(kDefaultThreshold, 26200, "Fragment ref with complex transform")},
                                    {"link-to-an-element-with-transform.svg", Params::WithThreshold(kDefaultThreshold, 34200, "Fragment ref with skewX transform on element")},
                                    {"preserveAspectRatio=none.svg", Params::WithThreshold(0.05f, 100, "Bilinear interpolation + sRGB↔linear roundtrip")},
                                    {"simple-case.svg", Params::Skip("External file reference (no ResourceLoader)")},
                                    {"svg.svg", Params::WithGoldenOverride("donner/svg/renderer/testdata/golden/resvg-svg.png").withReason("We render higher quality")},
                                    {"with-subregion-1.svg", Params::WithThreshold(kDefaultThreshold, 5100, "OBB subregion bilinear")},
                                    {"with-subregion-2.svg", Params::WithThreshold(kDefaultThreshold, 5100, "OBB subregion percentage")},
                                    {"with-subregion-3.svg", Params::WithThreshold(kDefaultThreshold, 14500, "Percentage width subregion")},
                                    {"with-subregion-4.svg", Params::WithThreshold(kDefaultThreshold, 15000, "Absolute subregion coords")},
                                    {"with-subregion-5.svg", Params::Skip("Subregion with rotation: filter")},
                                
                                    {"with-x-y-and-protruding-subregion-1.svg", Params::Skip("Bug: feImage edge cases / unsupported subregion combinations")},
                                    {"with-x-y-and-protruding-subregion-2.svg", Params::Skip("Bug: feImage edge cases / unsupported subregion combinations")},
                                    {"with-x-y.svg", Params::Skip("Bug: feImage edge cases / unsupported subregion combinations")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFeMerge, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("filters/feMerge",
                                {
                                    {"complex-transform.svg", Params::WithThreshold(0.15f, kDefaultMismatchedPixels, "Minor blur shading differences")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(FiltersFeMorphology, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("filters/feMorphology",
                                {
                                    {"empty-radius.svg", Params::Skip("Bug: feMorphology edge cases (empty radius, non-numeric radius)")},
                                    {"negative-radius.svg", Params::Skip("Bug: feMorphology edge cases (empty radius, non-numeric radius)")},
                                    {"no-radius.svg", Params::Skip("Bug: feMorphology edge cases (empty radius, non-numeric radius)")},
                                    {"radius-with-too-many-values.svg", Params::Skip("Bug: feMorphology edge cases (empty radius, non-numeric radius)")},
                                    {"zero-radius.svg", Params::Skip("Bug: feMorphology edge cases (empty radius, non-numeric radius)")},
                                })),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(FiltersFeOffset, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("filters/feOffset")),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFePointLight, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("filters/fePointLight",
                                {
                                    {"complex-transform.svg", Params::WithThreshold(0.1f, 120, "Minor shading differences")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFeSpecularLighting, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("filters/feSpecularLighting",
                                {
                                    {"with-fePointLight.svg", Params::WithGoldenOverride("donner/svg/renderer/testdata/golden/resvg-with-fePointLight.png", 0.02f).withReason("resvg golden")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFeSpotLight, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("filters/feSpotLight",
                                {
                                    {"complex-transform.svg", Params::WithGoldenOverride("donner/svg/renderer/testdata/golden/resvg-complex-transform.png").withReason("resvg bug: SpotLight Y")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFeTile, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("filters/feTile",
                                {
                                    {"complex-transform.svg", Params::RenderOnly("UB: complex transform")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFeTurbulence, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("filters/feTurbulence",
                                {
                                    {"color-interpolation-filters=sRGB.svg", Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "Minor shading differences")},
                                    {"complex-transform.svg", Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "Minor shading differences")},
                                    {"stitchTiles=stitch.svg", Params::RenderOnly("UB: stitchTiles=stitch")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFilter, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("filters/filter",
                                {
                                    {"complex-order-and-xlink-href.svg", Params::Skip("Bug: Color is slightly off, we are missing transparency")},
                                    {"in=BackgroundAlpha-with-enable-background.svg", Params::Skip("in=BackgroundAlpha (deprecated SVG 1.1)")},
                                    {"in=BackgroundImage-with-enable-background.svg", Params::Skip("in=BackgroundImage (deprecated SVG 1.1)")},
                                    {"in=FillPaint-on-g-without-children.svg", Params::RenderOnly("UB: in=FillPaint on empty group")},
                                    {"in=FillPaint-with-gradient.svg", Params::RenderOnly("UB: in=FillPaint gradient")},
                                    {"in=FillPaint-with-pattern.svg", Params::RenderOnly("UB: in=FillPaint pattern")},
                                    {"in=FillPaint-with-target-on-g.svg", Params::RenderOnly("UB: in=FillPaint on group")},
                                    {"in=FillPaint.svg", Params::RenderOnly("UB: in=FillPaint")},
                                    {"in=StrokePaint.svg", Params::RenderOnly("UB: in=StrokePaint")},
                                    {"on-the-root-svg.svg", Params::RenderOnly("UB: Filter on the root `svg`")},
                                    {"transform-on-shape-with-filter-region.svg", Params::Skip("Bug: We don't blur the right edge")},
                                    {"with-subregion-3.svg", Params::WithThreshold(0.1f, kDefaultMismatchedPixels, "Minor shading differences")},
                                
                                    {"content-outside-the-canvas-2.svg", Params::Skip("Bug: <filter> edge cases (filterRes, filterUnits, multiple inputs)")},
                                    {"in=BackgroundAlpha.svg", Params::Skip("Bug: <filter> edge cases (filterRes, filterUnits, multiple inputs)")},
                                    {"with-mask-on-parent.svg", Params::Skip("Bug: <filter> edge cases (filterRes, filterUnits, multiple inputs)")},
                                    {"with-transform-outside-of-canvas.svg", Params::Skip("Bug: <filter> edge cases (filterRes, filterUnits, multiple inputs)")},
                                })),
    TestNameFromFilename);

// TODO: The entire filters/filter-functions category produces "Data corrupted"
// parse errors on CI x86_64 runners but passes locally on aarch64. The root
// cause is unknown — possibly a resvg test suite data integrity issue on CI,
// or an x86_64-specific parser bug. Disabled until investigated.
//
// INSTANTIATE_TEST_SUITE_P(
//     FiltersFilterFunctions, ImageComparisonTestFixture,
//     ValuesIn(getTestsInCategory("filters/filter-functions")),
//     TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FiltersFloodColor, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("filters/flood-color",
                                {
                                    {"inheritance-3.svg", Params::Skip("230K diff: ICC color")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(FiltersFloodOpacity, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("filters/flood-opacity")),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    MaskingClip, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("masking/clip",
                                {
                                    {"simple-case.svg", Params::Skip()},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(MaskingClipRule, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("masking/clip-rule")),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    MaskingClipPath, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("masking/clipPath",
                                {
                                    {"clip-path-on-children.svg", Params::Skip("Bug: Nested clip-path not working")},
                                    {"clip-path-with-transform-on-text.svg", Params::Skip("Not impl: clipPath on <text>")},
                                    {"clipping-with-complex-text-1.svg", Params::Skip("Not impl: clipPath with <text> children")},
                                    {"clipping-with-complex-text-2.svg", Params::Skip("Not impl: clipPath with <text> children")},
                                    {"clipping-with-complex-text-and-clip-rule.svg", Params::Skip("Not impl: clipPath with <text> children")},
                                    {"clipping-with-text.svg", Params::Skip("Not impl: clipPath with <text> children")},
                                    {"on-the-root-svg-without-size.svg", Params::RenderOnly("UB: on root `<svg>` without size")},
                                    {"switch-is-not-a-valid-child.svg", Params::Skip("Not impl: <switch>")},
                                    {"with-use-child.svg", Params::Skip("Not impl: <use> child")},
                                
                                    {"circle-shorthand-with-stroke-box.svg", Params::Skip("Bug: clipPath edge cases beyond core support")},
                                    {"circle-shorthand-with-view-box.svg", Params::Skip("Bug: clipPath edge cases beyond core support")},
                                    {"circle-shorthand.svg", Params::Skip("Bug: clipPath edge cases beyond core support")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    MaskingMask, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("masking/mask",
                                {
                                    {"color-interpolation=linearRGB.svg", Params::Skip("Not implemented: color-interpolation linearRGB")},
                                    {"mask-on-self.svg", Params::Skip("Non-text mask regression kept out of text stack")},
                                    {"recursive-on-child.svg", Params::RenderOnly("UB: Recursive on child")},
                                    {"with-image.svg", Params::WithThreshold(0.1f, kDefaultMismatchedPixels, "Mask with <image> (bilinear edge diffs)")},
                                
                                    {"half-width-region-with-rotation.svg", Params::Skip("Bug: mask edge cases (color-interpolation, mask-units, mask-type) need investigation")},
                                    {"mask-on-self-with-mask-type=alpha.svg", Params::Skip("Bug: mask edge cases (color-interpolation, mask-units, mask-type) need investigation")},
                                    {"mask-on-self-with-mixed-mask-type.svg", Params::Skip("Bug: mask edge cases (color-interpolation, mask-units, mask-type) need investigation")},
                                    {"mask-type-in-style.svg", Params::Skip("Bug: mask edge cases (color-interpolation, mask-units, mask-type) need investigation")},
                                    {"mask-type=alpha.svg", Params::Skip("Bug: mask edge cases (color-interpolation, mask-units, mask-type) need investigation")},
                                    {"on-group-with-transform.svg", Params::Skip("Bug: mask edge cases (color-interpolation, mask-units, mask-type) need investigation")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintServersLinearGradient, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("paint-servers/linearGradient",
                                {
                                    {"invalid-gradientTransform.svg", Params::RenderOnly("UB: Invalid `gradientTransform`")},
                                
                                    {"gradientUnits=userSpaceOnUse-with-percent.svg", Params::Skip("Bug: intrinsic sizing + percent resolution with non-square viewBox; see ShapesEllipse")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintServersPattern, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("paint-servers/pattern",
                                {
                                    {"invalid-patternTransform.svg", Params::RenderOnly("UB: Invalid patternTransform")},
                                    {"out-of-order-referencing.svg", Params::WithThreshold(0.6f, 800, "Nested pattern AA (768px)")},
                                    {"overflow=visible.svg", Params::RenderOnly("UB: overflow=visible")},
                                    {"pattern-on-child.svg", Params::WithThreshold(0.2f, kDefaultMismatchedPixels, "Anti-aliasing artifacts")},
                                    {"patternContentUnits-with-viewBox.svg", Params::WithThreshold(kDefaultThreshold, 150, "Skia pattern AA")},
                                    {"patternContentUnits=objectBoundingBox.svg", Params::WithThreshold(kDefaultThreshold, 250, "Skia pattern AA")},
                                    {"recursive-on-child.svg", Params::WithThreshold(0.2f, kDefaultMismatchedPixels, "Larger threshold due to recursive pattern seams.")},
                                    {"self-recursive-on-child.svg", Params::WithThreshold(0.2f, kDefaultMismatchedPixels, "Larger threshold due to recursive pattern seams.")},
                                    {"self-recursive.svg", Params::WithThreshold(0.2f, kDefaultMismatchedPixels, "Larger threshold due to recursive pattern seams.")},
                                    {"text-child.svg", Params::WithThreshold(0.5f, 1150, "AA artifacts + quad glyph outlines")},
                                    {"tiny-pattern-upscaled.svg", Params::WithThreshold(0.02f, kDefaultMismatchedPixels, "Has anti-aliasing artifacts.")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintServersRadialGradient, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("paint-servers/radialGradient",
                                {
                                    {"focal-point-correction.svg", Params::Skip("Test suite bug? In SVG2 this was changed to draw")},
                                    {"fr=-1.svg", Params::RenderOnly("UB: fr=-1 (SVG 2)")},
                                    {"fr=0.5.svg", Params::RenderOnly("UB: fr=0.5 (SVG 2)")},
                                    {"fr=0.7.svg", Params::Skip("Test suite bug? fr > default value of")},
                                    {"invalid-gradientTransform.svg", Params::RenderOnly("UB: Invalid `gradientTransform`")},
                                    {"invalid-gradientUnits.svg", Params::RenderOnly("UB: Invalid `gradientUnits`")},
                                    {"negative-r.svg", Params::RenderOnly("UB: Negative `r`")},
                                
                                    {"gradientUnits=objectBoundingBox-with-percent.svg", Params::Skip("Bug: intrinsic sizing + percent resolution with non-square viewBox; see ShapesEllipse")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintServersStop, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("paint-servers/stop",
                                {
                                    {"stop-color-with-inherit-1.svg", Params::Skip("Bug? Strange edge case, stop-color")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintServersStopColor, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("paint-servers/stop-color")),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintServersStopOpacity, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("paint-servers/stop-opacity")),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintingColor, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("painting/color")),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintingContext, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("painting/context",
                                {
                                    {"in-marker.svg", Params::Skip("Not impl: context-fill / context-stroke (parsed but not honored at render)")},
                                    {"in-nested-marker.svg", Params::Skip("Not impl: context-fill / context-stroke (parsed but not honored at render)")},
                                    {"in-nested-use-and-marker.svg", Params::Skip("Not impl: context-fill / context-stroke (parsed but not honored at render)")},
                                    {"on-shape-with-zero-size-bbox.svg", Params::Skip("Not impl: context-fill / context-stroke (parsed but not honored at render)")},
                                    {"with-gradient-and-gradient-transform.svg", Params::Skip("Not impl: context-fill / context-stroke (parsed but not honored at render)")},
                                    {"with-gradient-in-use.svg", Params::Skip("Not impl: context-fill / context-stroke (parsed but not honored at render)")},
                                    {"with-gradient-on-marker.svg", Params::Skip("Not impl: context-fill / context-stroke (parsed but not honored at render)")},
                                    {"with-pattern-and-transform-in-use.svg", Params::Skip("Not impl: context-fill / context-stroke (parsed but not honored at render)")},
                                    {"with-pattern-in-use.svg", Params::Skip("Not impl: context-fill / context-stroke (parsed but not honored at render)")},
                                    {"with-pattern-objectBoundingBox-in-use.svg", Params::Skip("Not impl: context-fill / context-stroke (parsed but not honored at render)")},
                                    {"with-pattern-on-marker.svg", Params::Skip("Not impl: context-fill / context-stroke (parsed but not honored at render)")},
                                    {"with-text.svg", Params::Skip("Not impl: context-fill / context-stroke (parsed but not honored at render)")},
                                })),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintingDisplay, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("painting/display",
                                {
                                    {"none-on-tref.svg", Params::Skip("Not impl: <tref>")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintingFill, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("painting/fill",
                                {
                                    {"icc-color.svg", Params::RenderOnly("UB: ICC color")},
                                    {"linear-gradient-on-text.svg", Params::WithThreshold(kDefaultThreshold, 500)},
                                    {"pattern-on-text.svg", Params::WithThreshold(kDefaultThreshold, 2100)},
                                    {"radial-gradient-on-text.svg", Params::WithThreshold(kDefaultThreshold, 500)},
                                    {"rgb-int-int-int.svg", Params::RenderOnly("UB: rgb(int int int)")},
                                    {"valid-FuncIRI-with-a-fallback-ICC-color.svg", Params::Skip("Not impl: Fallback with icc-color")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintingFillOpacity, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("painting/fill-opacity")),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintingFillRule, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("painting/fill-rule")),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintingImageRendering, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("painting/image-rendering",
                                {
                                    {"on-feImage.svg", Params::Skip("Not impl: image-rendering property (pixelated/crisp-edges/smooth)")},
                                    {"optimizeSpeed.svg", Params::Skip("Not impl: image-rendering property (pixelated/crisp-edges/smooth)")},
                                })),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintingIsolation, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("painting/isolation")),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintingMarker, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("painting/marker",
                                {
                                    {"marker-on-text.svg", Params::Skip("Not impl: `text`")},
                                    {"orient=auto-on-M-C-C-4.svg", Params::WithGoldenOverride( "donner/svg/renderer/testdata/golden/resvg-orient=auto-on-M-C-C-4.png")},
                                    {"orient=auto-on-M-L-L-Z-Z-Z.svg", Params::Skip("Bug: Multiple closepaths")},
                                    {"orient=auto-on-M-L-Z.svg", Params::WithGoldenOverride( "donner/svg/renderer/testdata/golden/resvg-orient=auto-on-M-L-Z.png").withReason("BUG? Disagreement about marker direction on cusp")},
                                    {"target-with-subpaths-2.svg", Params::RenderOnly("UB: Target with subpaths")},
                                    {"with-a-text-child.svg", Params::WithThreshold(kDefaultThreshold, 110, "Minor AA diffs on Skia text_full")},
                                    {"with-an-image-child.svg", Params::WithGoldenOverride("donner/svg/renderer/testdata/golden/resvg-with-an-image-child.png").withReason("We (correctly)")},
                                    {"with-viewBox-1.svg", Params::RenderOnly("UB: with `viewBox`")},
                                
                                    {"marker-on-rounded-rect.svg", Params::Skip("Bug: marker edge cases (rounded-rect path corners, recursive-5)")},
                                    {"percent-values.svg", Params::Skip("Bug: intrinsic sizing + percent resolution with non-square viewBox; see ShapesEllipse")},
                                    {"recursive-5.svg", Params::Skip("Bug: marker edge cases (rounded-rect path corners, recursive-5)")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintingMixBlendMode, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("painting/mix-blend-mode")),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintingOpacity, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("painting/opacity",
                                {
                                    {"50percent.svg", Params::Skip("Changed in css-color-4 to allow percentage in")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintingOverflow, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("painting/overflow")),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintingPaintOrder, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("painting/paint-order",
                                {
                                    {"fill-markers-stroke.svg", Params::Skip("Not impl: paint-order property (parsed name only, no rendering)")},
                                    {"markers-stroke.svg", Params::Skip("Not impl: paint-order property (parsed name only, no rendering)")},
                                    {"markers.svg", Params::Skip("Not impl: paint-order property (parsed name only, no rendering)")},
                                    {"on-text.svg", Params::Skip("Not impl: paint-order property (parsed name only, no rendering)")},
                                    {"on-tspan.svg", Params::Skip("Not impl: paint-order property (parsed name only, no rendering)")},
                                    {"stroke-markers-fill.svg", Params::Skip("Not impl: paint-order property (parsed name only, no rendering)")},
                                    {"stroke-markers.svg", Params::Skip("Not impl: paint-order property (parsed name only, no rendering)")},
                                    {"stroke.svg", Params::Skip("Not impl: paint-order property (parsed name only, no rendering)")},
                                })),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintingShapeRendering, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("painting/shape-rendering")),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintingStroke, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("painting/stroke",
                                {
                                    {"linear-gradient-on-text.svg", Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "AA artifacts")},
                                    {"pattern-on-text.svg", Params::WithThreshold(0.1f, kDefaultMismatchedPixels, "AA artifacts")},
                                    {"radial-gradient-on-text.svg", Params::Skip("Bug: Gradient stroke on text")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintingStrokeDasharray, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("painting/stroke-dasharray",
                                {
                                    {"multiple-subpaths.svg", Params::WithThreshold(0.13f, kDefaultMismatchedPixels, "Larger threshold due to anti-aliasing artifacts.")},
                                    {"negative-sum.svg", Params::RenderOnly("UB (negative sum)")},
                                    {"negative-values.svg", Params::RenderOnly("UB (negative values)")},
                                
                                    {"0-n-with-butt-caps.svg", Params::Skip("Bug: stroke-dasharray edge cases (specific value patterns)")},
                                    {"0-n-with-round-caps.svg", Params::Skip("Bug: stroke-dasharray edge cases (specific value patterns)")},
                                    {"0-n-with-square-caps.svg", Params::Skip("Bug: stroke-dasharray edge cases (specific value patterns)")},
                                    {"n-0.svg", Params::Skip("Bug: stroke-dasharray edge cases (specific value patterns)")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintingStrokeDashoffset, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("painting/stroke-dashoffset")),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintingStrokeLinecap, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("painting/stroke-linecap")),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintingStrokeLinejoin, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("painting/stroke-linejoin",
                                {
                                    {"arcs.svg", Params::RenderOnly("UB (SVG 2), no UA supports `arcs`")},
                                    {"miter-clip.svg", Params::RenderOnly("UB (SVG 2), no UA supports `miter-clip`")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintingStrokeMiterlimit, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("painting/stroke-miterlimit")),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(PaintingStrokeOpacity, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("painting/stroke-opacity")),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintingStrokeWidth, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("painting/stroke-width",
                                {
                                    {"negative.svg", Params::RenderOnly("UB: Nothing should be rendered")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    PaintingVisibility, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("painting/visibility",
                                {
                                    {"bbox-impact-3.svg", Params::Skip("Not impl: <text> contributing to bbox handling")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(ShapesCircle, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("shapes/circle")),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    ShapesEllipse, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory(
        "shapes/ellipse",
        {
            // Bug: SVG has viewBox="0 0 200 100" with no width/height. Donner's
            // intrinsic document sizing computes 500x375 (wrong) instead of
            // 500x250, and percent-valued geometry (cx=50%, ry=20%, ...) then
            // resolves against that mis-sized viewport, so the ellipse is
            // larger than the golden and shifted. Root cause traced to
            // LayoutSystem::calculateRawDocumentSize's use of transformPosition
            // (which folds in the aspect-ratio letterbox translation) instead
            // of transformVector; fixing it in isolation breaks the percent
            // resolution pipeline downstream, so deferring to a dedicated PR.
            {"percent-values-missing-ry.svg",
             Params::Skip("Bug: intrinsic sizing + percent resolution with "
                          "non-square viewBox")},
            {"percent-values.svg",
             Params::Skip("Bug: intrinsic sizing + percent resolution with "
                          "non-square viewBox")},
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    ShapesLine, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory(
        "shapes/line",
        {
            {"simple-case.svg",
             Params::WithThreshold(0.02f, kDefaultMismatchedPixels,
                                   "Larger threshold due to anti-aliasing")},
            // Bug: see ShapesEllipse — non-square viewBox + percent geometry.
            {"percent-units.svg",
             Params::Skip("Bug: intrinsic sizing + percent resolution with "
                          "non-square viewBox")},
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(ShapesPath, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("shapes/path")),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(ShapesPolygon, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("shapes/polygon")),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(ShapesPolyline, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("shapes/polyline")),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    ShapesRect, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory(
        "shapes/rect",
        {
            // Bug: see ShapesEllipse — non-square viewBox + percent geometry.
            {"percentage-values-1.svg",
             Params::Skip("Bug: intrinsic sizing + percent resolution with "
                          "non-square viewBox")},
            {"percentage-values-2.svg",
             Params::Skip("Bug: intrinsic sizing + percent resolution with "
                          "non-square viewBox")},
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(StructureA, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("structure/a",
                                {
                                    {"inside-text.svg", Params::Skip("Not impl: <a> link element rendering / hyperlink processing")},
                                    {"inside-tspan.svg", Params::Skip("Not impl: <a> link element rendering / hyperlink processing")},
                                    {"on-tspan.svg", Params::Skip("Not impl: <a> link element rendering / hyperlink processing")},
                                })),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StructureDefs, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("structure/defs",
                                {
                                    {"style-inheritance-on-text.svg", Params::WithThreshold(kDefaultThreshold, 6500)},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(StructureG, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("structure/g")),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StructureImage, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("structure/image",
                                {
                                    {"float-size.svg", Params::RenderOnly("UB: Float size")},
                                    {"no-height-on-svg.svg", Params::RenderOnly("UB: No height")},
                                    {"no-width-and-height-on-svg.svg", Params::RenderOnly("UB: No width and height")},
                                    {"no-width-on-svg.svg", Params::RenderOnly("UB: No width")},
                                    {"url-to-png.svg", Params::Skip("Not impl: External URLs")},
                                    {"url-to-svg.svg", Params::Skip("Not impl: External URLs")},
                                
                                    {"embedded-16bit-png.svg", Params::Skip("Bug: <image> rendering layout/sizing differs from golden (embedded data URLs render but at wrong size; preserveAspectRatio modes need investigation)")},
                                    {"embedded-gif.svg", Params::Skip("Bug: <image> rendering layout/sizing differs from golden (embedded data URLs render but at wrong size; preserveAspectRatio modes need investigation)")},
                                    {"embedded-jpeg-without-mime.svg", Params::Skip("Bug: <image> rendering layout/sizing differs from golden (embedded data URLs render but at wrong size; preserveAspectRatio modes need investigation)")},
                                    {"embedded-png.svg", Params::Skip("Bug: <image> rendering layout/sizing differs from golden (embedded data URLs render but at wrong size; preserveAspectRatio modes need investigation)")},
                                    {"embedded-svg-with-text.svg", Params::Skip("Bug: <image> rendering layout/sizing differs from golden (embedded data URLs render but at wrong size; preserveAspectRatio modes need investigation)")},
                                    {"external-gif.svg", Params::Skip("Bug: <image> rendering layout/sizing differs from golden (embedded data URLs render but at wrong size; preserveAspectRatio modes need investigation)")},
                                    {"external-png.svg", Params::Skip("Bug: <image> rendering layout/sizing differs from golden (embedded data URLs render but at wrong size; preserveAspectRatio modes need investigation)")},
                                    {"no-height-non-square.svg", Params::Skip("Bug: <image> rendering layout/sizing differs from golden (embedded data URLs render but at wrong size; preserveAspectRatio modes need investigation)")},
                                    {"no-height.svg", Params::Skip("Bug: <image> rendering layout/sizing differs from golden (embedded data URLs render but at wrong size; preserveAspectRatio modes need investigation)")},
                                    {"no-width-and-height.svg", Params::Skip("Bug: <image> rendering layout/sizing differs from golden (embedded data URLs render but at wrong size; preserveAspectRatio modes need investigation)")},
                                    {"no-width.svg", Params::Skip("Bug: <image> rendering layout/sizing differs from golden (embedded data URLs render but at wrong size; preserveAspectRatio modes need investigation)")},
                                    {"preserveAspectRatio=none.svg", Params::Skip("Bug: <image> rendering layout/sizing differs from golden (embedded data URLs render but at wrong size; preserveAspectRatio modes need investigation)")},
                                    {"preserveAspectRatio=xMaxYMax-meet.svg", Params::Skip("Bug: <image> rendering layout/sizing differs from golden (embedded data URLs render but at wrong size; preserveAspectRatio modes need investigation)")},
                                    {"preserveAspectRatio=xMidYMid-meet.svg", Params::Skip("Bug: <image> rendering layout/sizing differs from golden (embedded data URLs render but at wrong size; preserveAspectRatio modes need investigation)")},
                                    {"preserveAspectRatio=xMinYMin-meet.svg", Params::Skip("Bug: <image> rendering layout/sizing differs from golden (embedded data URLs render but at wrong size; preserveAspectRatio modes need investigation)")},
                                    {"raster-image-and-size-with-odd-numbers.svg", Params::Skip("Bug: <image> rendering layout/sizing differs from golden (embedded data URLs render but at wrong size; preserveAspectRatio modes need investigation)")},
                                    {"width-and-height-set-to-auto.svg", Params::Skip("Bug: <image> rendering layout/sizing differs from golden (embedded data URLs render but at wrong size; preserveAspectRatio modes need investigation)")},
                                    {"with-transform.svg", Params::Skip("Bug: <image> rendering layout/sizing differs from golden (embedded data URLs render but at wrong size; preserveAspectRatio modes need investigation)")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StructureStyle, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("structure/style",
                                {
                                    {"external-CSS.svg", Params::Skip("Not impl: CSS @import")},
                                    {"non-presentational-attribute.svg", Params::Skip("Not impl: <svg version=\"1.1\">")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StructureStyleAttribute, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("structure/style-attribute",
                                {
                                    {"non-presentational-attribute.svg", Params::Skip("<svg version=\"1.1\"> disables geometry attributes in style")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StructureSvg, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("structure/svg",
                                {
                                    {"attribute-value-via-ENTITY-reference.svg", Params::Skip("Bug/Not impl? XML Entity references")},
                                    {"elements-via-ENTITY-reference-2.svg", Params::Skip("Bug/Not impl? XML Entity references")},
                                    {"elements-via-ENTITY-reference-3.svg", Params::Skip("Bug/Not impl? XML Entity references")},
                                    {"funcIRI-parsing.svg", Params::RenderOnly("UB: FuncIRI parsing")},
                                    {"funcIRI-with-invalid-characters.svg", Params::RenderOnly("UB: FuncIRI with invalid chars")},
                                    {"invalid-id-attribute-1.svg", Params::RenderOnly("UB: Invalid id attribute")},
                                    {"invalid-id-attribute-2.svg", Params::RenderOnly("UB: Invalid id attribute")},
                                    {"mixed-namespaces.svg", Params::Skip("Bug? mixed namespaces")},
                                    {"nested-svg-with-overflow-auto.svg", Params::Skip("Not impl: overflow")},
                                    {"nested-svg-with-overflow-visible.svg", Params::Skip("Not impl: overflow")},
                                    {"no-size.svg", Params::Skip("Not impl: Computed bounds from content")},
                                    {"not-UTF-8-encoding.svg", Params::Skip("Bug/Not impl? Non-UTF8 encoding")},
                                    {"preserveAspectRatio-with-viewBox-not-at-zero-pos.svg", Params::WithThreshold(0.13f, kDefaultMismatchedPixels, "Has anti-aliasing artifacts.")},
                                    {"preserveAspectRatio=none.svg", Params::WithThreshold(0.13f, kDefaultMismatchedPixels, "Has anti-aliasing artifacts.")},
                                    {"preserveAspectRatio=xMaxYMax-slice.svg", Params::WithThreshold(0.13f, kDefaultMismatchedPixels, "Has anti-aliasing artifacts.")},
                                    {"preserveAspectRatio=xMaxYMax.svg", Params::WithThreshold(0.13f, kDefaultMismatchedPixels, "Has anti-aliasing artifacts.")},
                                    {"preserveAspectRatio=xMidYMid-slice.svg", Params::WithThreshold(0.13f, kDefaultMismatchedPixels, "Has anti-aliasing artifacts.")},
                                    {"preserveAspectRatio=xMidYMid.svg", Params::WithThreshold(0.13f, kDefaultMismatchedPixels, "Has anti-aliasing artifacts.")},
                                    {"preserveAspectRatio=xMinYMin-slice.svg", Params::WithThreshold(0.13f, kDefaultMismatchedPixels, "Has anti-aliasing artifacts.")},
                                    {"preserveAspectRatio=xMinYMin.svg", Params::WithThreshold(0.13f, kDefaultMismatchedPixels, "Has anti-aliasing artifacts.")},
                                    {"proportional-viewBox.svg", Params::WithThreshold(0.13f, kDefaultMismatchedPixels, "Has anti-aliasing artifacts.")},
                                    {"rect-inside-a-non-SVG-element.svg", Params::Skip("Bug? Rect inside unknown element")},
                                    {"viewBox-not-at-zero-pos.svg", Params::WithThreshold(0.13f, kDefaultMismatchedPixels, "Has anti-aliasing artifacts.")},
                                    {"xmlns-validation.svg", Params::Skip("Bug? xmlns validation")},
                                
                                    {"funcIRI-with-quotes.svg", Params::Skip("Bug: <svg> root element edge cases")},
                                    {"nested-svg-one-with-rect-and-one-with-viewBox.svg", Params::Skip("Bug: <svg> root element edge cases")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(StructureSwitch, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("structure/switch",
                                {
                                    {"comment-as-first-child.svg", Params::Skip("Not impl: <switch>")},
                                    {"display-none-on-child.svg", Params::Skip("Not impl: <switch>")},
                                    {"non-SVG-child.svg", Params::Skip("Not impl: <switch>")},
                                    {"requiredFeatures.svg", Params::Skip("Not impl: <switch>")},
                                    {"simple-case.svg", Params::Skip("Not impl: <switch>")},
                                    {"systemLanguage.svg", Params::Skip("Not impl: <switch>")},
                                    {"systemLanguage=en-GB.svg", Params::Skip("Not impl: <switch>")},
                                    {"systemLanguage=en-US.svg", Params::Skip("Not impl: <switch>")},
                                    {"systemLanguage=en.svg", Params::Skip("Not impl: <switch>")},
                                    {"systemLanguage=ru-Ru.svg", Params::Skip("Not impl: <switch>")},
                                    {"systemLanguage=ru-en.svg", Params::Skip("Not impl: <switch>")},
                                    {"with-attributes.svg", Params::Skip("Not impl: <switch>")},
                                })),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StructureSymbol, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("structure/symbol",
                                {
                                    {"with-transform.svg", Params::Skip("New SVG2 feature, transform on symbol")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(StructureSystemLanguage, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("structure/systemLanguage",
                                {
                                    {"on-svg.svg", Params::Skip("Not impl: systemLanguage conditional processing")},
                                    {"on-tspan.svg", Params::Skip("Not impl: systemLanguage conditional processing")},
                                    {"ru-Ru.svg", Params::Skip("Not impl: systemLanguage conditional processing")},
                                })),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StructureTransform, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("structure/transform",
                                {
                                    {"rotate-at-position.svg", Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "Larger threshold due to anti-aliasing artifacts.")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StructureTransformOrigin, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory(
        "structure/transform-origin",
        {
            // TODO(#514): transform-origin rendering is broken after #514's
            // single-keyword parsing changes — 10K-150K pixel diffs indicate
            // the rendering is completely wrong, not just slightly off. Disabled
            // until the root cause is investigated and fixed properly.
            {"bottom.svg", Params::Skip("transform-origin broken after #514")},
            {"center.svg", Params::Skip("transform-origin broken after #514")},
            {"keyword-length.svg", Params::Skip("transform-origin broken after #514")},
            {"left.svg", Params::Skip("transform-origin broken after #514")},
            {"length-percent.svg", Params::Skip("transform-origin broken after #514")},
            {"length-px.svg", Params::Skip("transform-origin broken after #514")},
            {"on-clippath-objectBoundingBox.svg", Params::Skip("transform-origin broken after #514")},
            {"on-clippath.svg", Params::Skip("transform-origin broken after #514")},
            {"on-gradient-object-bounding-box.svg", Params::Skip("transform-origin broken after #514")},
            {"on-gradient-user-space-on-use.svg", Params::Skip("transform-origin broken after #514")},
            {"on-group.svg", Params::Skip("transform-origin broken after #514")},
            {"on-image.svg", Params::Skip("transform-origin broken after #514")},
            {"on-pattern-object-bounding-box.svg", Params::Skip("transform-origin broken after #514")},
            {"on-pattern-user-space-on-use.svg", Params::Skip("transform-origin broken after #514")},
            {"on-shape.svg", Params::Skip("transform-origin broken after #514")},
            {"on-text-path.svg", Params::Skip("transform-origin broken after #514")},
            {"on-text.svg", Params::Skip("transform-origin broken after #514")},
            {"right-bottom.svg", Params::Skip("transform-origin broken after #514")},
            {"right.svg", Params::Skip("transform-origin broken after #514")},
            {"top.svg", Params::Skip("transform-origin broken after #514")},
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StructureUse, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("structure/use",
                                {
                                    {"xlink-to-an-external-file.svg", Params::Skip("Not impl: External file.")},
                                
                                    {"nested-xlink-to-svg-element-with-rect-and-size.svg", Params::Skip("Bug: <use> referencing inline <svg> elements with various width/height/viewBox combinations")},
                                    {"xlink-to-svg-element-with-rect-only-width.svg", Params::Skip("Bug: <use> referencing inline <svg> elements with various width/height/viewBox combinations")},
                                    {"xlink-to-svg-element-with-rect.svg", Params::Skip("Bug: <use> referencing inline <svg> elements with various width/height/viewBox combinations")},
                                    {"xlink-to-svg-element-with-viewBox.svg", Params::Skip("Bug: <use> referencing inline <svg> elements with various width/height/viewBox combinations")},
                                    {"xlink-to-svg-element-with-width-height-on-use.svg", Params::Skip("Bug: <use> referencing inline <svg> elements with various width/height/viewBox combinations")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(TextAlignmentBaseline, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("text/alignment-baseline",
                                {
                                    {"after-edge.svg", Params::Skip("Not impl: full alignment-baseline keyword set + tspan baseline alignment")},
                                    {"baseline.svg", Params::Skip("Not impl: full alignment-baseline keyword set + tspan baseline alignment")},
                                    {"before-edge.svg", Params::Skip("Not impl: full alignment-baseline keyword set + tspan baseline alignment")},
                                    {"hanging-on-vertical.svg", Params::Skip("Not impl: full alignment-baseline keyword set + tspan baseline alignment")},
                                    {"ideographic.svg", Params::Skip("Not impl: full alignment-baseline keyword set + tspan baseline alignment")},
                                    {"middle-on-textPath.svg", Params::Skip("Not impl: full alignment-baseline keyword set + tspan baseline alignment")},
                                    {"middle.svg", Params::Skip("Not impl: full alignment-baseline keyword set + tspan baseline alignment")},
                                    {"text-after-edge.svg", Params::Skip("Not impl: full alignment-baseline keyword set + tspan baseline alignment")},
                                    {"text-before-edge.svg", Params::Skip("Not impl: full alignment-baseline keyword set + tspan baseline alignment")},
                                    {"two-textPath-with-middle-on-first.svg", Params::Skip("Not impl: full alignment-baseline keyword set + tspan baseline alignment")},
                                })),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextBaselineShift, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("text/baseline-shift",
                                {
                                    {"nested-with-baseline-1.svg", Params::WithThreshold(0.1f, kDefaultMismatchedPixels, "Minor AA artifacts on axis")},
                                    {"nested-with-baseline-2.svg", Params::WithThreshold(0.1f, kDefaultMismatchedPixels, "Minor AA artifacts on axis")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(TextDirection, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("text/direction",
                                {
                                    {"rtl-with-vertical-writing-mode.svg", Params::Skip("Not impl: direction property (BiDi text shaping)")},
                                    {"rtl.svg", Params::Skip("Not impl: direction property (BiDi text shaping)")},
                                })),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(TextDominantBaseline, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("text/dominant-baseline",
                                {
                                    {"alignment-baseline-and-baseline-shift-on-tspans.svg", Params::Skip("Not impl: full dominant-baseline keyword set (incl. before/after-edge, no-change, reset-size, use-script)")},
                                    {"alignment-baseline=baseline-on-tspan.svg", Params::Skip("Not impl: full dominant-baseline keyword set (incl. before/after-edge, no-change, reset-size, use-script)")},
                                    {"complex.svg", Params::Skip("Not impl: full dominant-baseline keyword set (incl. before/after-edge, no-change, reset-size, use-script)")},
                                    {"dummy-tspan.svg", Params::Skip("Not impl: full dominant-baseline keyword set (incl. before/after-edge, no-change, reset-size, use-script)")},
                                    {"hanging.svg", Params::Skip("Not impl: full dominant-baseline keyword set (incl. before/after-edge, no-change, reset-size, use-script)")},
                                    {"inherit.svg", Params::Skip("Not impl: full dominant-baseline keyword set (incl. before/after-edge, no-change, reset-size, use-script)")},
                                    {"middle.svg", Params::Skip("Not impl: full dominant-baseline keyword set (incl. before/after-edge, no-change, reset-size, use-script)")},
                                    {"nested.svg", Params::Skip("Not impl: full dominant-baseline keyword set (incl. before/after-edge, no-change, reset-size, use-script)")},
                                    {"no-change.svg", Params::Skip("Not impl: full dominant-baseline keyword set (incl. before/after-edge, no-change, reset-size, use-script)")},
                                    {"reset-size.svg", Params::Skip("Not impl: full dominant-baseline keyword set (incl. before/after-edge, no-change, reset-size, use-script)")},
                                    {"sequential.svg", Params::Skip("Not impl: full dominant-baseline keyword set (incl. before/after-edge, no-change, reset-size, use-script)")},
                                    {"text-after-edge.svg", Params::Skip("Not impl: full dominant-baseline keyword set (incl. before/after-edge, no-change, reset-size, use-script)")},
                                    {"text-before-edge.svg", Params::Skip("Not impl: full dominant-baseline keyword set (incl. before/after-edge, no-change, reset-size, use-script)")},
                                    {"use-script.svg", Params::Skip("Not impl: full dominant-baseline keyword set (incl. before/after-edge, no-change, reset-size, use-script)")},
                                })),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextFont, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("text/font",
                                {
                                    {"simple-case.svg", Params::Skip("Canvas size mismatch (400 vs 500)")},
                                
                                    {"font-shorthand.svg", Params::Skip("Not impl: font shorthand property")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextFontFamily, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("text/font-family",
                                {
                                    {"bold-sans-serif.svg", Params::WithThreshold(kDefaultThreshold, 5200, "Bold sans-serif (Noto Sans Bold)")},
                                    {"cursive.svg", Params::WithThreshold(kDefaultThreshold, 5000, "cursive (Yellowtail)")},
                                    {"fallback-1.svg", Params::Skip("Fallback from invalid family (different")},
                                    {"fallback-2.svg", Params::WithThreshold(kDefaultThreshold, 1000, "Fallback list: \"Invalid, Noto Sans\"")},
                                    {"fantasy.svg", Params::WithThreshold(kDefaultThreshold, 5200, "fantasy (Sedgwick Ave Display)")},
                                    {"font-list.svg", Params::WithThreshold(kDefaultThreshold, 1300, "Font list: Source Sans Pro fallback")},
                                    {"monospace.svg", Params::WithThreshold(kDefaultThreshold, 600, "monospace (Noto Mono)")},
                                    {"sans-serif.svg", Params::WithThreshold(kDefaultThreshold, 1900, "sans-serif (Noto Sans)")},
                                    {"serif.svg", Params::WithThreshold(kDefaultThreshold, 4200, "serif (Noto Serif)")},
                                    {"source-sans-pro.svg", Params::WithThreshold(kDefaultThreshold, 1300, "Source Sans Pro")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(TextFontKerning, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("text/font-kerning",
                                {
                                    {"arabic-script.svg", Params::Skip("Not impl: font-kerning property (HarfBuzz feature toggle)")},
                                    {"none.svg", Params::Skip("Not impl: font-kerning property (HarfBuzz feature toggle)")},
                                })),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextFontSize, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("text/font-size",
                                {
                                    {"named-value-without-a-parent.svg", Params::WithGoldenOverride( "donner/svg/renderer/testdata/golden/resvg-named-value-without-a-parent.png").withReason("Donner uses CSS Fonts Level 4, which has")},
                                    {"named-value.svg", Params::WithGoldenOverride( "donner/svg/renderer/testdata/golden/resvg-named-value.png").withReason("Donner uses CSS Fonts Level 4, which has")},
                                    {"negative-size.svg", Params::RenderOnly("UB: negative font size")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(TextFontSizeAdjust, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("text/font-size-adjust",
                                {
                                    {"simple-case.svg", Params::Skip("Not impl: font-size-adjust property")},
                                })),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(TextFontStretch, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("text/font-stretch")),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(TextFontStyle, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("text/font-style")),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextFontVariant, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("text/font-variant",
                                {
                                    {"inherit.svg", Params().withSimpleTextMaxPixels(1200).withReason("small-caps is emulated with simple text")},
                                    {"small-caps.svg", Params().withSimpleTextMaxPixels(1200).withReason("small-caps is emulated with simple text")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(TextFontWeight, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("text/font-weight")),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(TextGlyphOrientationHorizontal, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("text/glyph-orientation-horizontal",
                                {
                                    {"simple-case.svg", Params::Skip("Not impl: glyph-orientation-horizontal (deprecated SVG 1.1)")},
                                })),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(TextGlyphOrientationVertical, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("text/glyph-orientation-vertical",
                                {
                                    {"simple-case.svg", Params::Skip("Not impl: glyph-orientation-vertical (deprecated SVG 1.1)")},
                                })),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(TextKerning, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("text/kerning",
                                {
                                    {"0.svg", Params::Skip("Not impl: kerning attribute (deprecated SVG 1.1)")},
                                    {"10percent.svg", Params::Skip("Not impl: kerning attribute (deprecated SVG 1.1)")},
                                })),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(TextLengthAdjust, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("text/lengthAdjust",
                                {
                                    {"text-on-path.svg", Params::Skip("Not impl: lengthAdjust attribute (parented to textLength)")},
                                    {"vertical.svg", Params::Skip("Not impl: lengthAdjust attribute (parented to textLength)")},
                                    {"with-underline.svg", Params::Skip("Not impl: lengthAdjust attribute (parented to textLength)")},
                                })),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextLetterSpacing, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("text/letter-spacing",
                                {
                                    {"large-negative.svg", Params::RenderOnly("UB: negative letter-spacing")},
                                    {"mixed-scripts.svg", Params::Skip("Needs BiDi: mixed LTR Latin + RTL Arabic in one span")},
                                    {"non-ASCII-character.svg", Params::Skip("Bug? We render with a different CJK glyph. Wrong font?")},
                                    {"on-Arabic.svg", Params().requireFeature(RendererBackendFeature::TextFull).withReason("Arabic text")},
                                
                                    {"filter-bbox.svg", Params::Skip("Bug: letter-spacing edge cases")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextText, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("text/text",
                                {
                                    {"bidi-reordering.svg", Params::Skip("Not impl: Bidirectional text shaping")},
                                    {"complex-grapheme-split-by-tspan.svg", Params::RenderOnly("UB: grapheme split by tspan")},
                                    {"complex-graphemes-and-coordinates-list.svg", Params::WithGoldenOverride("donner/svg/renderer/testdata/golden/resvg-complex-graphemes-and-coordinates-list.png") .onlyTextFull().withReason("Simple text can't compose combining marks")},
                                    {"complex-graphemes.svg", Params().onlyTextFull().withReason("Combining mark needs HarfBuzz")},
                                    {"compound-emojis-and-coordinates-list.svg", Params::WithGoldenOverride("donner/svg/renderer/testdata/golden/resvg-compound-emojis-and-coordinates-list.png", 0.1f) .withMaxPixelsDifferent(1100) .onlyTextFull().withReason("Emoji, Skia bitmap scaling differs from TinySkia")},
                                    {"compound-emojis.svg", Params::WithThreshold(0.2f, kDefaultMismatchedPixels, "Emoji, differences between resvg and our bitmap") .onlyTextFull()},
                                    {"emojis.svg", Params::WithThreshold(0.2f, kDefaultMismatchedPixels, "Emoji, differences between").onlyTextFull()},
                                    {"fill-rule=evenodd.svg", Params().onlyTextFull().withReason("Arabic text shaping requires text-full")},
                                    {"rotate-on-Arabic.svg", Params::WithGoldenOverride("donner/svg/renderer/testdata/golden/resvg-rotate-on-Arabic.png") .onlyTextFull().withReason("Arabic text shaping requires text-full,")},
                                    {"rotate-with-multiple-values-and-complex-text.svg", Params().onlyTextFull().withReason("Complex diatrics requires text-full")},
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
                                    {"zalgo.svg",
                                     Params()
                                         .withMaxPixelsDifferent(300)
                                         .onlyTextFull()
                                         .withReason("Complex diacritics; vertical-axis AA diff "
                                                     "not the focus of the test")},
                                
                                    {"filter-bbox.svg", Params::Skip("Bug: text rendering edge cases (mixed inline content, BiDi-adjacent)")},
                                    {"ligatures-handling-in-mixed-fonts-1.svg", Params::Skip("Bug: text rendering edge cases (mixed inline content, BiDi-adjacent)")},
                                    {"ligatures-handling-in-mixed-fonts-2.svg", Params::Skip("Bug: text rendering edge cases (mixed inline content, BiDi-adjacent)")},
                                    {"percent-value-on-dx-and-dy.svg", Params::Skip("Bug: intrinsic sizing + percent resolution with non-square viewBox; see ShapesEllipse")},
                                    {"percent-value-on-x-and-y.svg", Params::Skip("Bug: intrinsic sizing + percent resolution with non-square viewBox; see ShapesEllipse")},
                                    {"real-text-height.svg", Params::Skip("Bug: text rendering edge cases (mixed inline content, BiDi-adjacent)")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextTextAnchor, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("text/text-anchor",
                                {
                                    {"coordinates-list.svg", Params::WithThreshold(0.1f, kDefaultMismatchedPixels, "Axis AA artifacts")},
                                    {"on-tspan-with-arabic.svg", Params().requireFeature(RendererBackendFeature::TextFull)},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextTextDecoration, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("text/text-decoration",
                                {
                                    {"all-types-inline-comma-separated.svg", Params::WithThreshold(0.1f, kDefaultMismatchedPixels, "Minor AA diffs")},
                                    {"all-types-inline-no-spaces.svg", Params::WithThreshold(0.1f, kDefaultMismatchedPixels, "Minor AA diffs")},
                                    {"all-types-inline.svg", Params::WithThreshold(0.1f, kDefaultMismatchedPixels, "Minor AA diffs")},
                                    {"indirect.svg", Params::WithGoldenOverride( "donner/svg/renderer/testdata/golden/resvg-indirect.png")},
                                    {"tspan-decoration.svg", Params::WithThreshold(0.1f, kDefaultMismatchedPixels, "Minor AA diffs")},
                                    {"underline-with-rotate-list-4.svg", Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "Minor shading diffs")},
                                
                                    {"indirect-with-multiple-colors.svg", Params::Skip("Not impl: text-decoration full SVG2 support (line/style/color independent)")},
                                    {"with-textLength-on-a-single-character.svg", Params::Skip("Not impl: text-decoration full SVG2 support (line/style/color independent)")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(TextTextRendering, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("text/text-rendering")),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextTextLength, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("text/textLength",
                                {
                                    {"on-text-and-tspan.svg", Params::Skip("Bug? We compress slightly more than the golden")},
                                
                                    {"arabic-with-lengthAdjust.svg", Params::Skip("Not impl: textLength + lengthAdjust attribute (text stretching/compressing)")},
                                    {"arabic.svg", Params::Skip("Not impl: textLength + lengthAdjust attribute (text stretching/compressing)")},
                                    {"on-a-single-tspan.svg", Params::Skip("Not impl: textLength + lengthAdjust attribute (text stretching/compressing)")},
                                    {"zero.svg", Params::Skip("Not impl: textLength + lengthAdjust attribute (text stretching/compressing)")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextTextPath, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("text/textPath",
                                {
                                    {"closed-path.svg", Params::WithThreshold(0.1f, 400, "Minor AA diffs")},
                                    {"complex.svg", Params::Skip("Deferred: vertical + circular path")},
                                    {"dy-with-tiny-coordinates.svg", Params::WithGoldenOverride( "donner/svg/renderer/testdata/golden/resvg-dy-with-tiny-coordinates.png", 0.05f) .withMaxPixelsDifferent( 1100).withReason("AA + minor char advance diffs, different w/ text vs. text-full so")},
                                    {"link-to-rect.svg", Params::Skip("Not impl: link to rect (SVG 2)")},
                                    {"m-A-path.svg", Params::WithThreshold(0.05f, kDefaultMismatchedPixels, "AA artifacts")},
                                    {"m-L-Z-path.svg", Params::WithGoldenOverride( "donner/svg/renderer/testdata/golden/resvg-m-L-Z-path.png").withReason("Minor char")},
                                    {"method=stretch.svg", Params::Skip("Not impl: method=stretch")},
                                    {"mixed-children-1.svg", Params::WithGoldenOverride( "donner/svg/renderer/testdata/golden/resvg-mixed-children-1.png").withReason("AA diffs")},
                                    {"mixed-children-2.svg", Params::Skip("Bug: Kerning on textPath")},
                                    {"nested.svg", Params::WithGoldenOverride("donner/svg/renderer/testdata/golden/resvg-nested.png").withReason("Minor char")},
                                    {"path-with-ClosePath.svg", Params::WithGoldenOverride( "donner/svg/renderer/testdata/golden/resvg-path-with-ClosePath.png").withReason("Minor char")},
                                    {"side=right.svg", Params::Skip("Not impl: side=right (SVG 2)")},
                                    {"simple-case.svg", Params::WithGoldenOverride( "donner/svg/renderer/testdata/golden/resvg-simple-case.png").withReason("Minor char")},
                                    {"spacing=auto.svg", Params::Skip("Not impl: spacing=auto")},
                                    {"startOffset=-100.svg", Params::WithGoldenOverride( "donner/svg/renderer/testdata/golden/resvg-startOffset=-100.png").withReason("Minor char")},
                                    {"startOffset=10percent.svg", Params::WithGoldenOverride( "donner/svg/renderer/testdata/golden/resvg-startOffset=10percent.png").withReason("Minor char")},
                                    {"startOffset=30.svg", Params::WithGoldenOverride( "donner/svg/renderer/testdata/golden/resvg-startOffset=30.png").withReason("Minor char")},
                                    {"startOffset=5mm.svg", Params::WithGoldenOverride( "donner/svg/renderer/testdata/golden/resvg-startOffset=5mm.png").withReason("Minor char")},
                                    {"tspan-with-absolute-position.svg", Params::WithGoldenOverride( "donner/svg/renderer/testdata/golden/resvg-tspan-with-absolute-position.png").withReason("Minor char")},
                                    {"tspan-with-relative-position.svg", Params::WithGoldenOverride( "donner/svg/renderer/testdata/golden/resvg-tspan-with-relative-position.png").withReason("Minor char")},
                                    {"two-paths.svg", Params::WithGoldenOverride( "donner/svg/renderer/testdata/golden/resvg-two-paths.png").withReason("Minor char")},
                                    {"very-long-text.svg", Params::WithGoldenOverride( "donner/svg/renderer/testdata/golden/resvg-very-long-text.png").withReason("AA diffs")},
                                    {"with-baseline-shift-and-rotate.svg", Params::RenderOnly("UB: baseline-shift + rotate")},
                                    {"with-baseline-shift.svg", Params::WithGoldenOverride( "donner/svg/renderer/testdata/golden/resvg-with-baseline-shift.png").withReason("Minor char")},
                                    {"with-coordinates-on-text.svg", Params::WithGoldenOverride( "donner/svg/renderer/testdata/golden/resvg-with-coordinates-on-text.png").withReason("Minor char")},
                                    {"with-coordinates-on-textPath.svg", Params::WithGoldenOverride("donner/svg/renderer/testdata/golden/resvg-with-coordinates-on-textPath.png").withReason("Minor char")},
                                    {"with-filter.svg", Params::Skip("Not impl: filter on textPath")},
                                    {"with-invalid-path-and-xlink-href.svg", Params::Skip("Not impl: invalid path + href")},
                                    {"with-path-and-xlink-href.svg", Params::Skip("Not impl: path + xlink:href")},
                                    {"with-path.svg", Params::Skip("Not impl: path attr (SVG 2)")},
                                    {"with-rotate.svg", Params::WithGoldenOverride( "donner/svg/renderer/testdata/golden/resvg-with-rotate.png").withReason("Minor char")},
                                    {"with-text-anchor.svg", Params::WithGoldenOverride( "donner/svg/renderer/testdata/golden/resvg-with-text-anchor.png")},
                                    {"with-transform-on-a-referenced-path.svg", Params::WithGoldenOverride( "donner/svg/renderer/testdata/golden/resvg-with-transform-on-a-referenced-path.png").withReason("Minor char")},
                                    {"with-transform-outside-a-referenced-path.svg", Params::WithGoldenOverride( "donner/svg/renderer/testdata/golden/resvg-with-transform-outside-a-referenced-path.png").withReason("Minor char")},
                                    {"with-underline.svg", Params::WithGoldenOverride( "donner/svg/renderer/testdata/golden/resvg-with-underline.png").withReason("Minor char")},
                                    {"writing-mode=tb.svg", Params::Skip("Deferred: writing-mode=tb on textPath")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(TextTref, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("text/tref",
                                {
                                    {"link-to-a-complex-text.svg", Params::Skip("Not impl: <tref> (deprecated SVG 2)")},
                                    {"link-to-a-non-text-element.svg", Params::Skip("Not impl: <tref> (deprecated SVG 2)")},
                                    {"link-to-an-external-file-element.svg", Params::Skip("Not impl: <tref> (deprecated SVG 2)")},
                                    {"link-to-text.svg", Params::Skip("Not impl: <tref> (deprecated SVG 2)")},
                                    {"position-attributes.svg", Params::Skip("Not impl: <tref> (deprecated SVG 2)")},
                                    {"style-attributes.svg", Params::Skip("Not impl: <tref> (deprecated SVG 2)")},
                                    {"with-a-title-child.svg", Params::Skip("Not impl: <tref> (deprecated SVG 2)")},
                                    {"with-text.svg", Params::Skip("Not impl: <tref> (deprecated SVG 2)")},
                                    {"xml-space.svg", Params::Skip("Not impl: <tref> (deprecated SVG 2)")},
                                })),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextTspan, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("text/tspan",
                                {
                                    {"bidi-reordering.svg", Params::Skip("Not impl: BIDI reordering")},
                                    {"mixed-font-size.svg", Params::Skip("Bug: Handling kerning with font size changes")},
                                    {"mixed-xml-space-3.svg", Params::Skip("Whitespace-only text nodes lost in")},
                                    {"nested-rotate.svg", Params::Skip("Bug: Applying rotation indices across nested tspans")},
                                    {"nested-whitespaces.svg", Params().withMaxPixelsDifferent(400).withReason("Vertical axis has different AA")},
                                    {"tspan-bbox-2.svg", Params().withMaxPixelsDifferent(900).withReason("Crosshair thin-line AA + underline uses")},
                                    {"with-clip-path.svg", Params::Skip("Not impl: Interaction with `clip-path`")},
                                    {"with-filter.svg", Params::Skip("Not impl: Interaction with `filter`")},
                                    {"with-mask.svg", Params::Skip("Not impl: Interaction with `mask`")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(TextUnicodeBidi, ImageComparisonTestFixture,
                         ValuesIn(getTestsInCategory("text/unicode-bidi",
                                {
                                    {"bidi-override.svg", Params::Skip("Not impl: unicode-bidi property (BiDi override)")},
                                })),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextWordSpacing, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("text/word-spacing",
                                {
                                    {"large-negative.svg", Params::RenderOnly("UB: word-spacing=-10000")},
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextWritingMode, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("text/writing-mode",
                                {
                                    {"arabic-with-rl.svg", Params::Skip("Non-ascii text").onlyTextFull()},
                                    {"inheritance.svg", Params().withMaxPixelsDifferent(1500).withReason("Bug: Baseline is ~2px off compared to resvg")},
                                    {"japanese-with-tb.svg", Params().onlyTextFull().withMaxPixelsDifferent( 600).withReason("Non-ascii text, bug: y position is ~1px off")},
                                    {"mixed-languages-with-tb-and-underline.svg", Params::Skip("Non-ascii text, bug: underline not").onlyTextFull()},
                                    {"mixed-languages-with-tb.svg", Params::Skip("Non-ascii text, bug: mixed language").onlyTextFull()},
                                    {"tb-and-punctuation.svg", Params::Skip("Non-ascii text, bug: CJK punctuation").onlyTextFull()},
                                    {"tb-rl.svg", Params().withMaxPixelsDifferent(1500).withReason("Bug: Baseline is ~2px off compared to resvg")},
                                    {"tb-with-alignment.svg", Params().withMaxPixelsDifferent(1500).withReason("Bug: Baseline is ~2px off compared to resvg")},
                                    {"tb-with-dx-on-second-tspan.svg", Params::Skip("Bug: `writing-mode=tb` with `dx`")},
                                    {"tb-with-dx-on-tspan.svg", Params::Skip("Bug: `writing-mode=tb` with `dx`")},
                                    {"tb-with-dy-on-second-tspan.svg", Params::Skip("Bug: `writing-mode=tb` with `dy`")},
                                    {"tb-with-rotate-and-underline.svg", Params::RenderOnly("UB: tb with rotate and underline")},
                                    {"tb-with-rotate.svg", Params::RenderOnly("UB: tb with rotate")},
                                    {"tb.svg", Params().withMaxPixelsDifferent(1500).withReason("Bug: Baseline is ~2px off compared to resvg")},
                                
                                    {"vertical-lr.svg", Params::Skip("Bug: writing-mode edge cases beyond basic support")},
                                    {"vertical-rl.svg", Params::Skip("Bug: writing-mode edge cases beyond basic support")},
                                })),
    TestNameFromFilename);


}  // namespace donner::svg
