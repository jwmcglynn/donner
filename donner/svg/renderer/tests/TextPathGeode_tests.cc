#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "donner/base/tests/Runfiles.h"
#include "donner/svg/renderer/tests/ImageComparisonTestFixture.h"
#include "donner/svg/text/TextTier.h"

namespace donner::svg {
namespace {

class TextPathGeodeTest : public ImageComparisonTestFixture {
protected:
  /// Compares real Geode text rendering without the broad corpus capability gate.
  void compareTextPath(const std::string& svgPath, const std::string& goldenPath,
                       int maxMismatchedPixels = kDefaultMismatchedPixels) {
    const std::filesystem::path source = Runfiles::instance().Rlocation(svgPath);
    SVGDocument document = loadSVG(source.string().c_str(),
                                   Runfiles::instance().Rlocation("third_party/resvg-test-suite/"));
    ImageComparisonParams params =
        ImageComparisonParams::WithThreshold(kDefaultThreshold, maxMismatchedPixels);
    params.setCanvasSize(500, 500);
    const std::string golden = Runfiles::instance().Rlocation(goldenPath);
    renderAndCompare(document, source, golden.c_str(), params, ComparisonMode::GeodeGolden);
  }
};

TEST_F(TextPathGeodeTest, RelativeOffsets) {
  compareTextPath(
      "third_party/resvg-test-suite/tests/text/textPath/tspan-with-relative-position.svg",
      "donner/svg/renderer/testdata/golden/resvg-tspan-with-relative-position.png");
}

TEST_F(TextPathGeodeTest, TinyRelativeOffsets) {
  // Preserve the corpus budget for the remaining full-text small-font placement residual.
  const int pixelBudget = IsFullTextBuiltIn() ? 1100 : kDefaultMismatchedPixels;
  compareTextPath("third_party/resvg-test-suite/tests/text/textPath/dy-with-tiny-coordinates.svg",
                  "donner/svg/renderer/testdata/golden/resvg-dy-with-tiny-coordinates.png",
                  pixelBudget);
}

TEST_F(TextPathGeodeTest, LengthPlacement) {
  compareTextPath("third_party/resvg-test-suite/tests/text/lengthAdjust/text-on-path.svg",
                  "third_party/resvg-test-suite/tests/text/lengthAdjust/text-on-path.png");
}

}  // namespace
}  // namespace donner::svg
