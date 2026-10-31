/// @file
/// SVG 2 text-path layout controls independent of the conformance corpus.
#include <gmock/gmock.h>
#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/renderer/tests/RendererTestUtils.h"
#include "donner/svg/tests/ParserTestUtils.h"

namespace donner::svg {
namespace {

RendererBitmap RenderPathScene(std::string_view source) {
  auto document = instantiateSubtree(source, parser::SVGParser::Options(), Vector2i(220, 200));
  return RenderDocumentWithActiveBackend(document);
}

TEST(TextPathConvergence, VerticalStraightPathMatchesOrdinaryVerticalText) {
  for (std::string_view attributes : {"", "textLength='80' lengthAdjust='spacingAndGlyphs'"}) {
    SCOPED_TRACE(attributes);
    const std::string style =
        " font-size='24' writing-mode='vertical-rl' " + std::string(attributes);
    const auto actual = RenderPathScene("<path id='p' d='M80 30V190'/><text" + style +
                                        "><textPath href='#p'>AV</textPath></text>");
    const auto expected = RenderPathScene("<text x='80' y='30'" + style + ">AV</text>");
    ASSERT_THAT(actual.pixels, testing::Not(testing::IsEmpty()));
    ASSERT_THAT(expected.dimensions, testing::Eq(Vector2i(220, 200)));
    const auto transparent = RenderPathScene("");
    EXPECT_NONFATAL_FAILURE(
        editor::tests::CompareBitmapToBitmap(expected, transparent, "vertical_path_visible",
                                             editor::tests::PixelmatchIdentityParams()),
        "pixels differ");
    editor::tests::CompareBitmapToBitmap(actual, expected, "vertical_path_frame",
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(TextPathConvergence, MixedChildrenContinueAtEachGeometricEndpoint) {
  const auto actual = RenderPathScene(
      "<defs><path id='p' d='M10 25H60'/><path id='q' d='M120 85V130'/></defs>"
      "<text font-size='18'><textPath href='#p'>A</textPath>"
      "<tspan fill='green'>V</tspan><textPath href='#q'>B</textPath>W</text>");
  const auto expected = RenderPathScene(
      "<text font-size='18'><textPath path='M10 25H60'>A</textPath>"
      "<tspan x='60' y='25' fill='green'>V</tspan>"
      "<textPath path='M120 85V130'>B</textPath><tspan x='120' y='130'>W</tspan></text>");
  ASSERT_THAT(actual.pixels, testing::Not(testing::IsEmpty()));
  ASSERT_THAT(expected.dimensions, testing::Eq(Vector2i(220, 200)));
  const auto transparent = RenderPathScene("");
  EXPECT_NONFATAL_FAILURE(
      editor::tests::CompareBitmapToBitmap(expected, transparent, "mixed_path_visible",
                                           editor::tests::PixelmatchIdentityParams()),
      "pixels differ");
  editor::tests::CompareBitmapToBitmap(actual, expected, "mixed_path_endpoints",
                                       editor::tests::PixelmatchIdentityParams());
}

}  // namespace
}  // namespace donner::svg
