#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <string_view>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/tests/Runfiles.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/tests/ImageComparisonTestFixture.h"
#include "donner/svg/renderer/tests/RendererTestBackend.h"
#include "donner/svg/renderer/tests/RgbaTestMatchers.h"

namespace donner::svg {
namespace {

SVGDocument ParseVerticalTextDocument(std::string_view source) {
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto result = parser::SVGParser::ParseSVG(source, warnings);
  EXPECT_FALSE(result.hasError()) << result.error();
  if (result.hasError()) {
    return SVGDocument();
  }

  SVGDocument document = std::move(result.result());
  RegisterFontsFromDirectoryForTesting(
      document, Runfiles::instance().Rlocation("third_party/resvg-test-suite/fonts"));
  return document;
}

RendererBitmap RenderVerticalText(std::string_view source) {
  SVGDocument document = ParseVerticalTextDocument(source);
  return RenderDocumentWithActiveBackend(document);
}

TEST(VerticalTextGeodeTest, BaselineShiftMatchesExplicitCrossAxisPosition) {
  constexpr std::string_view kShifted = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="128" height="128">
      <text x="80" y="16" font-family="Mplus 1p" font-size="20"
            writing-mode="vertical-rl" text-orientation="sideways"
            alignment-baseline="central"><tspan baseline-shift="4px">AB</tspan></text>
    </svg>)svg";
  // CSS Writing Modes maps positive baseline-shift to line-over, which is physical right in
  // vertical text. The independently positioned reference therefore adds exactly 4px to x.
  constexpr std::string_view kExplicitPosition = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="128" height="128">
      <text x="84" y="16" font-family="Mplus 1p" font-size="20"
            writing-mode="vertical-rl" text-orientation="sideways"
            alignment-baseline="central"><tspan>AB</tspan></text>
    </svg>)svg";
  const RendererBitmap shifted = RenderVerticalText(kShifted);
  const RendererBitmap explicitPosition = RenderVerticalText(kExplicitPosition);
  ASSERT_GT(test::CountNonTransparentPixels(shifted), 0u);
  ASSERT_GT(test::CountNonTransparentPixels(explicitPosition), 0u);
  editor::tests::CompareBitmapToBitmap(shifted, explicitPosition,
                                       "vertical-baseline-shift-position",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(VerticalTextGeodeTest, HangingBaselineMatchesExplicitAlphabeticPosition) {
  constexpr std::string_view kHanging = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="128" height="128"
         viewBox="0 0 6000 6000">
      <text x="5000" y="200" font-family="Noto Sans" font-size="5000"
            writing-mode="vertical-rl" text-orientation="sideways">
        <tspan alignment-baseline="hanging">A</tspan>
      </text>
    </svg>)svg";
  // NotoSans-Regular has unitsPerEm=1000 and hhea ascent=1069. At font-size 5000,
  // its em scale is exactly 5 and the synthesized hanging baseline is
  // 0.8 * 1069 * 5 = 4276 document units. The equivalent alphabetic position is
  // therefore x=5000-4276=724.
  constexpr std::string_view kExplicitPosition = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="128" height="128"
         viewBox="0 0 6000 6000">
      <text x="724" y="200" font-family="Noto Sans" font-size="5000"
            writing-mode="vertical-rl" text-orientation="sideways">
        <tspan alignment-baseline="alphabetic">A</tspan>
      </text>
    </svg>)svg";

  const RendererBitmap hanging = RenderVerticalText(kHanging);
  const RendererBitmap explicitPosition = RenderVerticalText(kExplicitPosition);
  ASSERT_GT(test::CountNonTransparentPixels(hanging), 0u);
  ASSERT_GT(test::CountNonTransparentPixels(explicitPosition), 0u);
  editor::tests::CompareBitmapToBitmap(hanging, explicitPosition,
                                       "vertical-hanging-baseline-position",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(VerticalTextGeodeTest, AutoEquivalentBaselineAliasesMatchAuto) {
  const auto source = [](std::string_view baseline) {
    return std::string(
               R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="128" height="128">
      <text x="80" y="16" font-family="Noto Sans" font-size="20"
            writing-mode="vertical-rl" text-orientation="sideways">
        <tspan dominant-baseline=")svg") +
           std::string(baseline) + R"svg(">AB</tspan>
      </text>
    </svg>)svg";
  };

  const RendererBitmap automatic = RenderVerticalText(source("auto"));
  ASSERT_GT(test::CountNonTransparentPixels(automatic), 0u);
  for (const std::string_view alias : {"use-script", "reset-size", "no-change"}) {
    const RendererBitmap aliased = RenderVerticalText(source(alias));
    ASSERT_GT(test::CountNonTransparentPixels(aliased), 0u);
    editor::tests::CompareBitmapToBitmap(aliased, automatic,
                                         std::string("vertical-baseline-") + std::string(alias),
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(VerticalTextGeodeTest, NoChangeKeepsResolvedParentHangingBaseline) {
  constexpr std::string_view kInherited = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="128" height="128"
         viewBox="0 0 6000 6000">
      <text x="5000" y="200" font-family="Noto Sans" font-size="5000"
            writing-mode="vertical-rl" text-orientation="sideways"
            dominant-baseline="hanging">
        <tspan dominant-baseline="no-change">A</tspan>
      </text>
    </svg>)svg";
  constexpr std::string_view kExplicit = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="128" height="128"
         viewBox="0 0 6000 6000">
      <text x="5000" y="200" font-family="Noto Sans" font-size="5000"
            writing-mode="vertical-rl" text-orientation="sideways">
        <tspan dominant-baseline="hanging">A</tspan>
      </text>
    </svg>)svg";

  const RendererBitmap inherited = RenderVerticalText(kInherited);
  const RendererBitmap explicitBaseline = RenderVerticalText(kExplicit);
  ASSERT_GT(test::CountNonTransparentPixels(inherited), 0u);
  ASSERT_GT(test::CountNonTransparentPixels(explicitBaseline), 0u);
  editor::tests::CompareBitmapToBitmap(inherited, explicitBaseline,
                                       "vertical-no-change-parent-hanging",
                                       editor::tests::PixelmatchIdentityParams());
}

}  // namespace
}  // namespace donner::svg
