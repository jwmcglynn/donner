/// @file
/// SVG 2 text-path layout controls independent of the conformance corpus.
#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/renderer/tests/RendererTestUtils.h"
#include "donner/svg/tests/ParserTestUtils.h"

namespace donner::svg {
namespace {

void ComparePathScene(std::string_view actual, std::string_view expected,
                      std::string_view checkpoint) {
  auto document = instantiateSubtree(actual, parser::SVGParser::Options(), Vector2i(220, 200));
  auto control = instantiateSubtree(expected, parser::SVGParser::Options(), Vector2i(220, 200));
  editor::tests::CompareBitmapToBitmap(RenderDocumentWithActiveBackend(document),
                                       RenderDocumentWithActiveBackend(control), checkpoint,
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(TextPathConvergence, VerticalStraightPathMatchesOrdinaryVerticalText) {
  for (std::string_view attributes : {"", "textLength='80' lengthAdjust='spacingAndGlyphs'"}) {
    SCOPED_TRACE(attributes);
    const std::string style =
        " font-size='24' writing-mode='vertical-rl' " + std::string(attributes);
    ComparePathScene(
        "<path id='p' d='M80 30V190'/><text" + style + "><textPath href='#p'>AV</textPath></text>",
        "<text x='80' y='30'" + style + ">AV</text>", "vertical_path_frame");
  }
}

TEST(TextPathConvergence, MixedChildrenContinueAtEachGeometricEndpoint) {
  ComparePathScene(
      "<defs><path id='p' d='M10 25H60'/><path id='q' d='M120 85V130'/></defs>"
      "<text font-size='18'><textPath href='#p'>A</textPath>"
      "<tspan fill='green'>V</tspan><textPath href='#q'>B</textPath>W</text>",
      "<text font-size='18'><textPath path='M10 25H60'>A</textPath>"
      "<tspan x='60' y='25' fill='green'>V</tspan>"
      "<textPath path='M120 85V130'>B</textPath><tspan x='120' y='130'>W</tspan></text>",
      "mixed_path_endpoints");
}

}  // namespace
}  // namespace donner::svg
