#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/svg/renderer/tests/RendererTestUtils.h"
#include "donner/svg/renderer/tests/RgbaTestMatchers.h"
#include "donner/svg/tests/ParserTestUtils.h"

namespace donner::svg {
namespace {

SVGDocument ParsePresentation(std::string_view source) {
  return instantiateSubtree(source, parser::SVGParser::Options(), Vector2i(16, 16));
}

TEST(RendererAnimationPresentation, PatternTileWidthRendersAndRestores) {
  auto document = ParsePresentation(R"svg(
    <defs><pattern id="p" patternUnits="userSpaceOnUse" width="4" height="16">
      <set attributeName="width" to="8" dur="1s"/>
      <rect width="2" height="16"/>
    </pattern></defs><rect width="16" height="16" fill="url(#p)"/>)svg");
  document.setTime(0.5);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("@@......@@......"));
  document.setTime(2);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("@@..@@..@@..@@.."));
}

TEST(RendererAnimationPresentation, FilterRegionWidthRendersAndRestores) {
  auto document = ParsePresentation(R"svg(
    <defs><filter id="f" filterUnits="userSpaceOnUse" x="0" y="0" width="4" height="16">
      <set attributeName="width" to="12" dur="1s"/><feOffset/>
    </filter></defs><rect width="16" height="16" filter="url(#f)"/>)svg");
  document.setTime(0.5);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("@@@@@@@@@@@@...."));
  document.setTime(2);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("@@@@............"));
}

TEST(RendererAnimationPresentation, FilterPrimitiveWidthRendersAndRestores) {
  auto document = ParsePresentation(R"svg(
    <defs><filter id="f" filterUnits="userSpaceOnUse" x="0" y="0" width="16" height="16">
      <feFlood x="0" y="0" width="4" height="16" flood-color="black">
        <set attributeName="width" to="12" dur="1s"/>
      </feFlood>
    </filter></defs><rect width="16" height="16" filter="url(#f)"/>)svg");
  document.setTime(0.5);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("@@@@@@@@@@@@...."));
  document.setTime(2);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("@@@@............"));
}

TEST(RendererAnimationPresentation, TextPositionMatchesAnIndependentStaticSample) {
  auto document = ParsePresentation(R"svg(
    <text x="0" y="12" font-size="12">H<set attributeName="x" to="8" dur="1s"/></text>)svg");
  auto expected = ParsePresentation(R"(<text x="8" y="12" font-size="12">H</text>)");
  auto base = ParsePresentation(R"(<text x="0" y="12" font-size="12">H</text>)");
  document.setTime(0.5);
  const auto expectedAscii = RendererTestUtils::renderToAsciiImage(expected).generated;
  EXPECT_THAT(expectedAscii, testing::HasSubstr("@"));
  EXPECT_EQ(RendererTestUtils::renderToAsciiImage(document).generated, expectedAscii);
  document.setTime(2);
  EXPECT_EQ(RendererTestUtils::renderToAsciiImage(document).generated,
            RendererTestUtils::renderToAsciiImage(base).generated);
}

TEST(RendererAnimationPresentation, EditedPatternWidthRestoresAuthorCascadePriority) {
  auto document = ParsePresentation(R"svg(
    <style>pattern { width: 6px; }</style>
    <defs><pattern id="p" patternUnits="userSpaceOnUse" width="4" height="16">
      <set attributeName="width" to="8" dur="1s"/><rect width="2" height="16"/>
    </pattern></defs><rect width="16" height="16" fill="url(#p)"/>)svg");
  document.setTime(0.5);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("@@......@@......"));
  document.querySelector("#p")->setAttribute("width", "12");
  document.setTime(2);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("@@....@@....@@.."));
}

TEST(RendererAnimationPresentation, MotionUsesCssDefinedPathGeometry) {
  auto document = ParsePresentation(R"svg(
    <defs><path id="route" d="M0 0L8 0" style="d: 'M0 0L0 8'"/></defs>
    <rect width="4" height="4"><animateMotion dur="2s"><mpath href="#route"/></animateMotion></rect>)svg");
  auto expected = ParsePresentation(R"(<rect y="4" width="4" height="4"/>)");
  document.setTime(1);
  EXPECT_EQ(RendererTestUtils::renderToAsciiImage(document).generated,
            RendererTestUtils::renderToAsciiImage(expected).generated);
}

TEST(RendererAnimationPresentation, TextLengthAndRotationMatchStaticSamples) {
  struct Case {
    std::string_view attribute;
    std::string_view value;
  };
  for (const Case item : {Case{"textLength", "12"}, Case{"rotate", "15"}}) {
    SCOPED_TRACE(item.attribute);
    const std::string opening = "<text x='2' y='12' font-size='12' lengthAdjust='spacingAndGlyphs'";
    auto document =
        ParsePresentation(opening + ">H<set attributeName='" + std::string(item.attribute) +
                          "' to='" + std::string(item.value) + "' dur='1s'/></text>");
    auto expected = ParsePresentation(opening + " " + std::string(item.attribute) + "='" +
                                      std::string(item.value) + "'>H</text>");
    auto base = ParsePresentation(opening + ">H</text>");
    document.setTime(0.5);
    const auto expectedAscii = RendererTestUtils::renderToAsciiImage(expected).generated;
    EXPECT_THAT(expectedAscii, testing::ContainsRegex("[^.\\n]"));
    EXPECT_EQ(RendererTestUtils::renderToAsciiImage(document).generated, expectedAscii);
    document.setTime(2);
    EXPECT_EQ(RendererTestUtils::renderToAsciiImage(document).generated,
              RendererTestUtils::renderToAsciiImage(base).generated);
  }
}

TEST(RendererAnimationPresentation, UseHrefSwitchesAndRestoresWithoutChangingSource) {
  auto document = ParsePresentation(R"svg(
    <defs><rect id="a" width="4" height="16"/><rect id="b" x="8" width="4" height="16"/></defs>
    <use href="#a"><set attributeName="href" to="#b" begin="0.5s" dur="1s"/></use>)svg");
  const std::string source(document.source());
  for (int cycle = 0; cycle < 3; ++cycle) {
    document.setTime(1);
    EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
                testing::StartsWith("........@@@@...."));
    document.setTime(2);
    EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
                testing::StartsWith("@@@@............"));
  }
  EXPECT_EQ(document.source(), source);
}

TEST(RendererAnimationPresentation, GradientHrefSwitchesInheritedStops) {
  auto document = ParsePresentation(R"svg(
    <defs><linearGradient id="a"><stop stop-color="black"/></linearGradient>
      <linearGradient id="b"><stop stop-color="red"/></linearGradient>
      <linearGradient id="g" href="#a"/>
      <set href="#g" attributeName="href" to="#b" begin="0.5s" dur="1s"/>
    </defs><rect width="16" height="16" fill="url(#g)"/>)svg");
  document.setTime(1);
  EXPECT_THAT(test::PixelAt(RenderDocumentWithActiveBackend(document), 8, 8),
              test::Rgba(255, 0, 0, 255));
  document.setTime(2);
  EXPECT_THAT(test::PixelAt(RenderDocumentWithActiveBackend(document), 8, 8),
              test::Rgba(0, 0, 0, 255));
}

TEST(RendererAnimationPresentation, PatternHrefSwitchesInheritedContent) {
  auto document = ParsePresentation(R"svg(
    <defs><pattern id="a" patternUnits="userSpaceOnUse" width="16" height="16"><rect width="4" height="16"/></pattern>
      <pattern id="b" patternUnits="userSpaceOnUse" width="16" height="16"><rect x="8" width="4" height="16"/></pattern>
      <pattern id="p" href="#a"/>
      <set href="#p" attributeName="href" to="#b" begin="0.5s" dur="1s"/>
    </defs><rect width="16" height="16" fill="url(#p)"/>)svg");
  document.setTime(1);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("........@@@@...."));
  document.setTime(2);
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(document).generated,
              testing::StartsWith("@@@@............"));
}

TEST(RendererAnimationPresentation, AnimatedMpathHrefChangesTheSameMotionSample) {
  auto document = ParsePresentation(R"svg(
    <defs><path id="a" d="M0 0L8 0"/><path id="b" d="M0 0L0 8"/></defs>
    <rect width="4" height="4"><animateMotion dur="2s"><mpath id="m" href="#a"/></animateMotion></rect>
    <set href="#m" attributeName="href" to="#b" begin="0.5s" dur="1s"/>)svg");
  auto expected = ParsePresentation(R"(<rect y="4" width="4" height="4"/>)");
  document.setTime(1);
  EXPECT_EQ(RendererTestUtils::renderToAsciiImage(document).generated,
            RendererTestUtils::renderToAsciiImage(expected).generated);
}

TEST(RendererAnimationPresentation, TextPathHrefSwitchesLayout) {
  auto document = ParsePresentation(R"svg(
    <defs><path id="a" d="M0 12H16"/><path id="b" d="M0 16H16"/></defs>
    <text font-size="12"><textPath id="t" href="#a">H</textPath></text>
    <set href="#t" attributeName="href" to="#b" begin="0.5s" dur="1s"/>)svg");
  auto expected = ParsePresentation(R"svg(
    <defs><path id="b" d="M0 16H16"/></defs>
    <text font-size="12"><textPath href="#b">H</textPath></text>)svg");
  document.setTime(1);
  EXPECT_EQ(RendererTestUtils::renderToAsciiImage(document).generated,
            RendererTestUtils::renderToAsciiImage(expected).generated);
}

}  // namespace
}  // namespace donner::svg
