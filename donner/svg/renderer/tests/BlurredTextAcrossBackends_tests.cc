#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <utility>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/tests/Runfiles.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/tests/ImageComparisonTestFixture.h"
#include "donner/svg/renderer/tests/RendererTestBackend.h"

namespace donner::svg {
namespace {

using testing::Eq;

/// One blurred run of text, alone on a transparent canvas, so the comparison sees only the filter's
/// input and output: no unfiltered edge lies under the blur.
struct BlurredTextCase {
  std::string_view name;
  std::string_view svg;
};

// Each scene scales a 200-unit view box to 500 px, so `stdDeviation="4"` blurs by 10 device px.
constexpr BlurredTextCase kCases[] = {
    {"text_on_curved_path", R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200" width="500" height="500">
        <defs>
          <filter id="blur"><feGaussianBlur stdDeviation="4"/></filter>
          <path id="wave" d="M 15 120 C 45 165 90 165 105 120 C 120 75 165 75 190 120"/>
        </defs>
        <text font-family="Noto Sans" font-size="22">
          <textPath href="#wave" filter="url(#blur)">Blurred words on a wave</textPath>
        </text>
      </svg>)svg"},
    {"tspan", R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200" width="500" height="500">
        <defs><filter id="blur"><feGaussianBlur stdDeviation="4"/></filter></defs>
        <text x="30" y="120" font-family="Noto Sans" font-size="60">
          <tspan filter="url(#blur)">Blur</tspan>
        </text>
      </svg>)svg"},
};

RendererBitmap RenderCase(const BlurredTextCase& testCase, RendererBackend backend) {
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(testCase.svg, warnings);
  EXPECT_FALSE(parsed.hasError()) << testCase.name << ": " << parsed.error();
  SVGDocument document = std::move(parsed.result());
  RegisterFontsFromDirectoryForTesting(
      document, Runfiles::instance().Rlocation("third_party/resvg-test-suite/fonts"));
  return RenderDocumentWithBackend(document, backend);
}

// Geode blurs in floating point and the CPU renderer rounds between its box passes, so the blurred
// alpha may differ by a level or two; the suite's default comparison (threshold 0.02) counts none
// of that. A difference in the blur kernel, filter region, glyph coverage fed to the blur or
// compositing of the result counts pixels here.
TEST(BlurredTextAcrossBackends, GeodeBlurMatchesCpuRenderer) {
  if (!IsRendererBackendAvailable(RendererBackend::Geode)) {
    GTEST_SKIP() << "Requires the Geode backend";
  }
  for (const BlurredTextCase& testCase : kCases) {
    SCOPED_TRACE(testCase.name);
    int counted = -1;
    editor::tests::CompareBitmapToBitmap(RenderCase(testCase, RendererBackend::Geode),
                                         RenderCase(testCase, RendererBackend::TinySkia),
                                         "blurred_text_geode_vs_cpu_" + std::string(testCase.name),
                                         {}, &counted);
    EXPECT_THAT(counted, Eq(0)) << "pixels pixelmatch counts between Geode and the CPU renderer";
  }
}

}  // namespace
}  // namespace donner::svg
