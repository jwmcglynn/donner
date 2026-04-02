#include <gtest/gtest.h>

#include <fstream>
#include <string>
#include <string_view>

#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/tests/ImageComparisonTestFixture.h"

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

class RendererTests : public ImageComparisonTestFixture {
protected:
  parser::SVGParser::Options optionsExperimental() {
    parser::SVGParser::Options options;
    options.enableExperimental = true;
    return options;
  }

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

    auto maybeResult = parser::SVGParser::ParseSVG(fileData, nullptr, options);
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
  this->compareWithGolden("donner/svg/renderer/testdata/minimal_closed_cubic_2x2.svg",
                          "donner/svg/renderer/testdata/golden/minimal_closed_cubic_2x2.png",
                          parser::SVGParser::Options(),
                          ImageComparisonParams::WithThreshold(0.0f, 0)
                              .includeAntiAliasingDifferences()
                              .setCanvasSize(10, 10));
}

TEST_F(RendererTests, MinimalClosedCubic5x3) {
  this->compareWithGolden("donner/svg/renderer/testdata/minimal_closed_cubic_5x3.svg",
                          "donner/svg/renderer/testdata/golden/minimal_closed_cubic_5x3.png",
                          parser::SVGParser::Options(),
                          ImageComparisonParams::WithThreshold(0.0f, 0)
                              .includeAntiAliasingDifferences()
                              .setCanvasSize(10, 6));
}

TEST_F(RendererTests, BigLightningGlowNoFilterCrop) {
  this->compareWithGolden(
      "donner/svg/renderer/testdata/big_lightning_glow_no_filter_crop.svg",
      "donner/svg/renderer/testdata/golden/big_lightning_glow_no_filter_crop.png",
      parser::SVGParser::Options(),
      ImageComparisonParams::WithThreshold(0.0f, 0).includeAntiAliasingDifferences());
}

TEST_F(RendererTests, DonnerIcon) {
  this->compareWithGolden("donner_icon.svg", "donner/svg/renderer/testdata/golden/donner_icon.png");
}

TEST_F(RendererTests, DonnerSplash) {
  this->compareWithGolden(
      "donner_splash.svg", "donner/svg/renderer/testdata/golden/donner_splash.png",
      this->optionsExperimental(),
      ImageComparisonParams::WithThreshold(0.1f).requireFeature(
          RendererBackendFeature::FilterEffects, "experimental filter effects"));
}

TEST_F(RendererTests, DonnerSplashNoExperimental) {
  this->compareWithGolden("donner_splash.svg",
                          "donner/svg/renderer/testdata/golden/donner_splash_no_experimental.png",
                          parser::SVGParser::Options(), ImageComparisonParams::WithThreshold(0.1f));
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

TEST_F(RendererTests, Edzample) {
  this->compareWithGolden("donner/svg/renderer/testdata/Edzample_Anim3.svg",
                          "donner/svg/renderer/testdata/golden/Edzample_Anim3.png",
                          this->optionsExperimental());
}

}  // namespace
}  // namespace donner::svg
