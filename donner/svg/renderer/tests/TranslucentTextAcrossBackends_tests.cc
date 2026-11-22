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

/// One run of blurred or masked text, alone on a transparent canvas, so the comparison sees only
/// the translucent text: no unfiltered edge lies under it.
struct TranslucentTextCase {
  std::string_view name;
  std::string_view svg;
};

// Each scene scales a 200-unit view box to 500 px, so `stdDeviation="4"` blurs by 10 device px.
constexpr TranslucentTextCase kCases[] = {
    {"blurred_text_on_curved_path", R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200" width="500" height="500">
        <defs>
          <filter id="blur"><feGaussianBlur stdDeviation="4"/></filter>
          <path id="wave" d="M 15 120 C 45 165 90 165 105 120 C 120 75 165 75 190 120"/>
        </defs>
        <text font-family="Noto Sans" font-size="22">
          <textPath href="#wave" filter="url(#blur)">Blurred words on a wave</textPath>
        </text>
      </svg>)svg"},
    {"blurred_tspan", R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200" width="500" height="500">
        <defs><filter id="blur"><feGaussianBlur stdDeviation="4"/></filter></defs>
        <text x="30" y="120" font-family="Noto Sans" font-size="60">
          <tspan filter="url(#blur)">Blur</tspan>
        </text>
      </svg>)svg"},
    {"masked_tspan", R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200" width="500" height="500">
        <defs>
          <mask id="half"><rect x="10" y="40" width="180" height="120" fill="#808080"/></mask>
        </defs>
        <text x="25" y="120" font-family="Noto Sans" font-size="60">
          <tspan mask="url(#half)">Mask</tspan>
        </text>
      </svg>)svg"},
};

void RenderCase(const TranslucentTextCase& testCase, RendererBackend backend,
                RendererBitmap* bitmap) {
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(testCase.svg, warnings);
  ASSERT_FALSE(parsed.hasError()) << testCase.name << ": " << parsed.error();
  SVGDocument document = std::move(parsed.result());
  RegisterFontsFromDirectoryForTesting(
      document, Runfiles::instance().Rlocation("third_party/resvg-test-suite/fonts"));
  *bitmap = RenderDocumentWithBackend(document, backend);
}

// Geode blurs in floating point and the CPU renderer rounds between its box passes, and the two
// cover glyph edges differently, so translucent pixels may differ by a few levels; the suite's
// default comparison (threshold 0.02) counts none of that on text alone. A difference in the blur
// kernel, filter region, mask luminance, glyph coverage fed to the effect or compositing of the
// result counts pixels here.
TEST(TranslucentTextAcrossBackends, GeodeMatchesCpuRenderer) {
  if (!IsRendererBackendAvailable(RendererBackend::Geode)) {
    GTEST_SKIP() << "Requires the Geode backend";
  }
  for (const TranslucentTextCase& testCase : kCases) {
    SCOPED_TRACE(testCase.name);
    RendererBitmap geode;
    RendererBitmap cpu;
    ASSERT_NO_FATAL_FAILURE(RenderCase(testCase, RendererBackend::Geode, &geode));
    ASSERT_NO_FATAL_FAILURE(RenderCase(testCase, RendererBackend::TinySkia, &cpu));
    int counted = -1;
    editor::tests::CompareBitmapToBitmap(
        geode, cpu, "translucent_text_geode_vs_cpu_" + std::string(testCase.name), {}, &counted);
    EXPECT_THAT(counted, Eq(0)) << "pixels pixelmatch counts between Geode and the CPU renderer";
  }
}

}  // namespace
}  // namespace donner::svg
