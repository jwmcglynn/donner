#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "donner/base/ParseWarningSink.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/tests/ImageComparisonTestFixture.h"
#include "donner/svg/renderer/tests/RendererTestBackend.h"
#include "donner/svg/resources/SandboxedFileResourceLoader.h"

// clang-format off
/**
 * @file
 *
 * Renderer image comparison tests, which render an SVG file and compare the result to a golden
 * image checked into the repo.
 *
 * If this test fails, update goldens with the following command:
 * ```sh
 * # Set this environment variable to the donner root directory
 * UPDATE_GOLDEN_IMAGES_DIR=$(bazel info workspace) bazel run //donner/svg/renderer/tests:renderer_tests
 * ```
 */
// clang-format on

namespace donner::svg {
namespace {

using Params = ImageComparisonParams;

/// Default per-pixel threshold for the Geode backend when a test doesn't have
/// an explicit override. Slug's per-pixel winding-number coverage produces
/// edge antialiasing that differs from tiny-skia's supersampling, so we run
/// with a wider default threshold and count mismatched pixels rather than
/// tightening per-pixel identity. Tests that need more slack than this
/// default should be listed explicitly in `geodeOverrides()` below with a
/// brief comment explaining the source of the divergence (which should be
/// treated as a bug to fix, not a permanent exception).
constexpr float kGeodeDefaultThreshold = 0.1f;
constexpr int kGeodeDefaultMaxMismatchedPixels = 2000;

/// Per-test Geode-backend overrides, keyed by the golden filename passed
/// into `compareWithGolden`. Same pattern as the resvg test suite's
/// per-test overrides map (`getTestsWithPrefix`).
///
/// Each entry here represents a known divergence between Geode and the
/// shared (tiny-skia-authored) golden. **A custom Geode golden should be
/// considered a bug** — the preferred override is a threshold bump with a
/// brief note explaining the root cause so we can fix it later. Use
/// `Params::WithGoldenOverride(...)` only as a last resort, and never add
/// a per-test golden under `testdata/golden/geode/` without a paired
/// TODO/issue reference. Use `requireFeature(...)` for tests that depend
/// on renderer features Geode doesn't implement yet.
const std::map<std::string_view, ImageComparisonParams>& geodeOverrides() {
  // Suite-wide: tests that need more slack than kGeodeDefault*.
  // Every entry is a Geode-only divergence we should eventually root-cause
  // and fix, at which point the entry should shrink or disappear.
  static const std::map<std::string_view, ImageComparisonParams> overrides = {
      // feImage pulls the external SVG through the Phase 7 filter engine,
      // which rasterizes the nested document into an intermediate texture
      // before compositing. Sample-pattern differences between Slug's 4×
      // MSAA and tiny-skia's 16× supersample accumulate along every edge
      // of the embedded SVG, so the per-pixel threshold alone isn't
      // enough — widen the mismatched-pixel cap to absorb the fringe.
      // Actual diff at 0.1 threshold: ~13.7k pixels.
      {"donner/svg/renderer/testdata/golden/feimage-external-svg.png",
       Params::WithThreshold(0.1f, 15000)},

      // Ghostscript Tiger is stroke-dense; whiskers + outlines accumulate
      // the 4× MSAA vs 16× supersample edge drift into several thousand
      // sub-threshold pixels. Actual diff at 0.1 threshold: ~3.9k pixels.
      {"donner/svg/renderer/testdata/golden/Ghostscript_Tiger.png",
       Params::WithThreshold(0.1f, 4500)},

      // Radial-conical focal-boundary divergence fixed: fragments outside the
      // gradient cone (disc < 0) are now discarded instead of painted with a
      // sentinel-derived stop color.  Both tests pass at default threshold.
      //
      // Image-external-SVG and use-external-SVG tests (image-external-svg-*,
      // use-external-svg*, nested-svg-aspectratio, z0rly_test6,
      // feimage-external-svg with FilterEffects always-on) previously carried
      // Geode-specific overrides; they now all render within the default
      // Geode threshold (0.1f / 2000 px) so the overrides were removed.
  };
  return overrides;
}

/// Apply Geode-backend overrides to the supplied params. When the active
/// backend is NOT Geode this is a no-op. When Geode is active:
///   - If the golden has an explicit entry in `geodeOverrides()`, use it.
///   - Otherwise, widen the default threshold to `kGeodeDefaultThreshold`
///     while preserving any explicitly-set canvas size / feature
///     requirements / backend gates from the caller's original params.
ImageComparisonParams applyGeodeOverrides(const char* goldenFilename,
                                          ImageComparisonParams params) {
  if (ActiveRendererBackend() != RendererBackend::Geode) {
    return params;
  }

  if (auto it = geodeOverrides().find(std::string_view(goldenFilename));
      it != geodeOverrides().end()) {
    return it->second;
  }

  // No explicit override: loosen the threshold and the mismatched-pixel cap
  // but keep everything else the caller specified. We want the test to
  // still pass/fail on structural correctness, just not on Slug-vs-tinyskia
  // sub-pixel AA noise.
  params.threshold = std::max(params.threshold, kGeodeDefaultThreshold);
  params.maxMismatchedPixels =
      std::max(params.maxMismatchedPixels, kGeodeDefaultMaxMismatchedPixels);
  return params;
}

class RendererTests : public ImageComparisonTestFixture {
protected:
  SVGDocument loadSVG(const char* filename, parser::SVGParser::Options options = {}) {
    std::ifstream file(filename);
    EXPECT_TRUE(file) << "Failed to open file: " << filename;
    if (!file) {
      return SVGDocument();
    }

    file.seekg(0, std::ios::end);
    const std::streamsize fileLength = file.tellg();
    file.seekg(0);

    std::string fileData;
    fileData.resize(fileLength);
    file.read(fileData.data(), fileLength);

    ParseWarningSink disabled = ParseWarningSink::Disabled();
    auto maybeResult = parser::SVGParser::ParseSVG(fileData, disabled, options);
    EXPECT_FALSE(maybeResult.hasError()) << "Parse Error: " << maybeResult.error();
    if (maybeResult.hasError()) {
      return SVGDocument();
    }

    return std::move(maybeResult.result());
  }

  void compareWithGolden(
      const char* svgFilename, const char* goldenFilename, parser::SVGParser::Options options = {},
      ImageComparisonParams params = ImageComparisonParams::WithThreshold(0.1f)) {
    SVGDocument document = loadSVG(svgFilename, options);
    params = applyGeodeOverrides(goldenFilename, params);
    params.enableGoldenUpdateFromEnv();
    renderAndCompare(document, svgFilename, goldenFilename, params);
  }

  SVGDocument loadSVGWithResources(const char* filename, parser::SVGParser::Options options = {}) {
    std::ifstream file(filename);
    EXPECT_TRUE(file) << "Failed to open file: " << filename;
    if (!file) {
      return SVGDocument();
    }

    file.seekg(0, std::ios::end);
    const std::streamsize fileLength = file.tellg();
    file.seekg(0);

    std::string fileData;
    fileData.resize(fileLength);
    file.read(fileData.data(), fileLength);

    const std::filesystem::path filePath(filename);
    const std::filesystem::path resourceDir = filePath.parent_path();

    SVGDocument::Settings settings;
    settings.resourceLoader = std::make_unique<SandboxedFileResourceLoader>(resourceDir, filePath);

    ParseWarningSink disabled = ParseWarningSink::Disabled();
    auto maybeResult =
        parser::SVGParser::ParseSVG(fileData, disabled, options, std::move(settings));
    EXPECT_FALSE(maybeResult.hasError()) << "Parse Error: " << maybeResult.error();
    if (maybeResult.hasError()) {
      return SVGDocument();
    }

    return std::move(maybeResult.result());
  }

  void compareWithGoldenAndResources(
      const char* svgFilename, const char* goldenFilename, parser::SVGParser::Options options = {},
      ImageComparisonParams params = ImageComparisonParams::WithThreshold(0.1f)) {
    SVGDocument document = loadSVGWithResources(svgFilename, options);
    params = applyGeodeOverrides(goldenFilename, params);
    params.enableGoldenUpdateFromEnv();
    renderAndCompare(document, svgFilename, goldenFilename, params);
  }
};

TEST_F(RendererTests, Ellipse1) {
  this->compareWithGolden("donner/svg/renderer/testdata/ellipse1.svg",
                          "donner/svg/renderer/testdata/golden/ellipse1.png");
}

TEST_F(RendererTests, Rect2) {
  this->compareWithGolden("donner/svg/renderer/testdata/rect2.svg",
                          "donner/svg/renderer/testdata/golden/rect2.png");
}

TEST_F(RendererTests, Skew1) {
  this->compareWithGolden("donner/svg/renderer/testdata/skew1.svg",
                          "donner/svg/renderer/testdata/golden/skew1.png");
}

TEST_F(RendererTests, SizeTooLarge) {
  this->compareWithGolden("donner/svg/renderer/testdata/size-too-large.svg",
                          "donner/svg/renderer/testdata/golden/size-too-large.png");
}

TEST_F(RendererTests, NestedSvgAspectRatio) {
  this->compareWithGolden("donner/svg/renderer/testdata/nested-svg-aspectratio.svg",
                          "donner/svg/renderer/testdata/golden/nested-svg-aspectratio.png",
                          parser::SVGParser::Options(), ImageComparisonParams::WithThreshold(0.1f));
}

TEST_F(RendererTests, RadialFr1) {
  this->compareWithGolden("donner/svg/renderer/testdata/radial-fr-1.svg",
                          "donner/svg/renderer/testdata/golden/radial-fr-1.png");
}

TEST_F(RendererTests, RadialFr2) {
  this->compareWithGolden("donner/svg/renderer/testdata/radial-fr-2.svg",
                          "donner/svg/renderer/testdata/golden/radial-fr-2.png");
}

TEST_F(RendererTests, RadialConical1) {
  this->compareWithGolden("donner/svg/renderer/testdata/radial-conical-1.svg",
                          "donner/svg/renderer/testdata/golden/radial-conical-1.png");
}

TEST_F(RendererTests, RadialConical2) {
  this->compareWithGolden("donner/svg/renderer/testdata/radial-conical-2.svg",
                          "donner/svg/renderer/testdata/golden/radial-conical-2.png");
}

TEST_F(RendererTests, GhostscriptTiger) {
  this->compareWithGolden("donner/svg/renderer/testdata/Ghostscript_Tiger.svg",
                          "donner/svg/renderer/testdata/golden/Ghostscript_Tiger.png",
                          parser::SVGParser::Options(),
                          ImageComparisonParams::WithThreshold(0.1f, 200));
}

TEST_F(RendererTests, Polygon) {
  this->compareWithGolden("donner/svg/renderer/testdata/polygon.svg",
                          "donner/svg/renderer/testdata/golden/polygon.png");
}

TEST_F(RendererTests, Polyline) {
  this->compareWithGolden("donner/svg/renderer/testdata/polyline.svg",
                          "donner/svg/renderer/testdata/golden/polyline.png");
}

TEST_F(RendererTests, Lion) {
  this->compareWithGolden("donner/svg/renderer/testdata/lion.svg",
                          "donner/svg/renderer/testdata/golden/lion.png");
}

TEST_F(RendererTests, StrokingComplex) {
  this->compareWithGolden("donner/svg/renderer/testdata/stroking_complex.svg",
                          "donner/svg/renderer/testdata/golden/stroking_complex.png");
}

TEST_F(RendererTests, StrokingDasharray) {
  this->compareWithGolden("donner/svg/renderer/testdata/stroking_dasharray.svg",
                          "donner/svg/renderer/testdata/golden/stroking_dasharray.png");
}

TEST_F(RendererTests, StrokingDashoffset) {
  this->compareWithGolden("donner/svg/renderer/testdata/stroking_dashoffset.svg",
                          "donner/svg/renderer/testdata/golden/stroking_dashoffset.png");
}

TEST_F(RendererTests, StrokingLinecap) {
  this->compareWithGolden("donner/svg/renderer/testdata/stroking_linecap.svg",
                          "donner/svg/renderer/testdata/golden/stroking_linecap.png");
}

TEST_F(RendererTests, StrokingLinejoin) {
  this->compareWithGolden("donner/svg/renderer/testdata/stroking_linejoin.svg",
                          "donner/svg/renderer/testdata/golden/stroking_linejoin.png");
}

TEST_F(RendererTests, StrokingMiterlimit) {
  this->compareWithGolden("donner/svg/renderer/testdata/stroking_miterlimit.svg",
                          "donner/svg/renderer/testdata/golden/stroking_miterlimit.png");
}

TEST_F(RendererTests, StrokingStrokewidth) {
  this->compareWithGolden("donner/svg/renderer/testdata/stroking_strokewidth.svg",
                          "donner/svg/renderer/testdata/golden/stroking_strokewidth.png");
}

TEST_F(RendererTests, StrokingPathLength) {
  this->compareWithGolden("donner/svg/renderer/testdata/stroking_pathlength.svg",
                          "donner/svg/renderer/testdata/golden/stroking_pathlength.png");
}

TEST_F(RendererTests, PokerChips) {
  this->compareWithGolden("donner/svg/renderer/testdata/poker_chips.svg",
                          "donner/svg/renderer/testdata/golden/poker_chips.png",
                          parser::SVGParser::Options(),
                          ImageComparisonParams::WithThreshold(0.1f, 1500));
}

TEST_F(RendererTests, QuadBezier) {
  this->compareWithGolden("donner/svg/renderer/testdata/quadbezier1.svg",
                          "donner/svg/renderer/testdata/golden/quadbezier1.png");
}

TEST_F(RendererTests, MinimalClosedCubic2x2) {
  this->compareWithGolden(
      "donner/svg/renderer/testdata/minimal_closed_cubic_2x2.svg",
      "donner/svg/renderer/testdata/golden/minimal_closed_cubic_2x2.png",
      parser::SVGParser::Options(),
      ImageComparisonParams::WithThreshold(0.0f, 0).includeAntiAliasingDifferences().setCanvasSize(
          10, 10));
}

TEST_F(RendererTests, MinimalClosedCubic5x3) {
  this->compareWithGolden(
      "donner/svg/renderer/testdata/minimal_closed_cubic_5x3.svg",
      "donner/svg/renderer/testdata/golden/minimal_closed_cubic_5x3.png",
      parser::SVGParser::Options(),
      ImageComparisonParams::WithThreshold(0.0f, 0).includeAntiAliasingDifferences().setCanvasSize(
          10, 6));
}

TEST_F(RendererTests, BigLightningGlowNoFilterCrop) {
  this->compareWithGolden(
      "donner/svg/renderer/testdata/big_lightning_glow_no_filter_crop.svg",
      "donner/svg/renderer/testdata/golden/big_lightning_glow_no_filter_crop.png",
      parser::SVGParser::Options(),
      ImageComparisonParams::WithThreshold(0.0f, 0).includeAntiAliasingDifferences());
}

TEST_F(RendererTests, FilterFillPaint) {
  this->compareWithGolden(
      "donner/svg/renderer/testdata/filter_fill_paint.svg",
      "donner/svg/renderer/testdata/golden/filter_fill_paint.png", parser::SVGParser::Options(),
      ImageComparisonParams::WithThreshold(0.1f, 50)
          .requireFeature(RendererBackendFeature::FilterEffects, "filter effects")
          .includeAntiAliasingDifferences());
}

TEST_F(RendererTests, FilterStrokePaint) {
  this->compareWithGolden(
      "donner/svg/renderer/testdata/filter_stroke_paint.svg",
      "donner/svg/renderer/testdata/golden/filter_stroke_paint.png", parser::SVGParser::Options(),
      ImageComparisonParams::WithThreshold(0.1f, 50)
          .requireFeature(RendererBackendFeature::FilterEffects, "filter effects")
          .includeAntiAliasingDifferences());
}

TEST_F(RendererTests, FilterDropShadow) {
  this->compareWithGolden(
      "donner/svg/renderer/testdata/filter_drop_shadow.svg",
      "donner/svg/renderer/testdata/golden/filter_drop_shadow.png", parser::SVGParser::Options(),
      ImageComparisonParams::WithThreshold(0.1f, 50)
          .requireFeature(RendererBackendFeature::FilterEffects, "filter effects")
          .includeAntiAliasingDifferences());
}

TEST_F(RendererTests, FilterSpotLight) {
  this->compareWithGolden(
      "donner/svg/renderer/testdata/filter_spot_light.svg",
      "donner/svg/renderer/testdata/golden/filter_spot_light.png", parser::SVGParser::Options(),
      ImageComparisonParams::WithThreshold(0.1f, 50)
          .requireFeature(RendererBackendFeature::FilterEffects, "filter effects")
          .includeAntiAliasingDifferences());
}

TEST_F(RendererTests, FilterDisplacementMap) {
  this->compareWithGolden(
      "donner/svg/renderer/testdata/filter_displacement_map.svg",
      "donner/svg/renderer/testdata/golden/filter_displacement_map.png",
      parser::SVGParser::Options(),
      ImageComparisonParams::WithThreshold(0.1f, 600)
          .requireFeature(RendererBackendFeature::FilterEffects, "filter effects")
          .includeAntiAliasingDifferences());
}

TEST_F(RendererTests, DonnerIcon) {
  this->compareWithGolden("donner_icon.svg", "donner/svg/renderer/testdata/golden/donner_icon.png");
}

TEST_F(RendererTests, DonnerSplash) {
  this->compareWithGolden("donner_splash.svg",
                          "donner/svg/renderer/testdata/golden/donner_splash.png",
                          parser::SVGParser::Options(),
                          ImageComparisonParams::WithThreshold(0.1f).requireFeature(
                              RendererBackendFeature::FilterEffects, "filter effects"));
}

TEST_F(RendererTests, SVG2_e_use_001) {
  this->compareWithGolden("donner/svg/renderer/testdata/svg2-e-use-001.svg",
                          "donner/svg/renderer/testdata/golden/svg2-e-use-001.png");
}

TEST_F(RendererTests, SVG2_e_use_002) {
  this->compareWithGolden("donner/svg/renderer/testdata/svg2-e-use-002.svg",
                          "donner/svg/renderer/testdata/golden/svg2-e-use-002.png");
}

TEST_F(RendererTests, SVG2_e_use_003) {
  this->compareWithGolden("donner/svg/renderer/testdata/svg2-e-use-003.svg",
                          "donner/svg/renderer/testdata/golden/svg2-e-use-003.png");
}

TEST_F(RendererTests, SVG2_e_use_004) {
  this->compareWithGolden("donner/svg/renderer/testdata/svg2-e-use-004.svg",
                          "donner/svg/renderer/testdata/golden/svg2-e-use-004.png");
}

TEST_F(RendererTests, SVG2_e_use_005) {
  this->compareWithGolden("donner/svg/renderer/testdata/svg2-e-use-005.svg",
                          "donner/svg/renderer/testdata/golden/svg2-e-use-005.png");
}

TEST_F(RendererTests, FeImageExternalSvg) {
  this->compareWithGoldenAndResources(
      "donner/svg/renderer/testdata/feimage-external-svg.svg",
      "donner/svg/renderer/testdata/golden/feimage-external-svg.png");
}

TEST_F(RendererTests, Edzample) {
  this->compareWithGolden("donner/svg/renderer/testdata/Edzample_Anim3.svg",
                          "donner/svg/renderer/testdata/golden/Edzample_Anim3.png");
}

TEST_F(RendererTests, ImageExternalSvgBasic) {
  this->compareWithGoldenAndResources(
      "donner/svg/renderer/testdata/image-external-svg-basic.svg",
      "donner/svg/renderer/testdata/golden/image-external-svg-basic.png");
}

TEST_F(RendererTests, ImageExternalSvgPar) {
  this->compareWithGoldenAndResources(
      "donner/svg/renderer/testdata/image-external-svg-par.svg",
      "donner/svg/renderer/testdata/golden/image-external-svg-par.png");
}

TEST_F(RendererTests, ImageExternalSvgViewbox) {
  this->compareWithGoldenAndResources(
      "donner/svg/renderer/testdata/image-external-svg-viewbox.svg",
      "donner/svg/renderer/testdata/golden/image-external-svg-viewbox.png");
}

TEST_F(RendererTests, UseExternalSvg) {
  this->compareWithGoldenAndResources("donner/svg/renderer/testdata/use-external-svg.svg",
                                      "donner/svg/renderer/testdata/golden/use-external-svg.png");
}

TEST_F(RendererTests, UseExternalSvgFragment) {
  this->compareWithGoldenAndResources(
      "donner/svg/renderer/testdata/use-external-svg-fragment.svg",
      "donner/svg/renderer/testdata/golden/use-external-svg-fragment.png");
}

TEST_F(RendererTests, UseExternalContextPaint) {
  this->compareWithGoldenAndResources(
      "donner/svg/renderer/testdata/use-external-context-paint.svg",
      "donner/svg/renderer/testdata/golden/use-external-context-paint.png");
}

TEST_F(RendererTests, Z0rlyTest6_MusicNotation) {
  this->compareWithGolden("donner/svg/renderer/testdata/z0rly_test6.svg",
                          "donner/svg/renderer/testdata/golden/z0rly_test6.png",
                          parser::SVGParser::Options());
}

}  // namespace
}  // namespace donner::svg
