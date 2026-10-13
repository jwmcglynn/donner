#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/renderer/tests/RendererTestUtils.h"
#include "donner/svg/tests/ParserTestUtils.h"

namespace donner::svg {
namespace {

TEST(RendererAnimationCoordinateContext, GradientUsesUnitsSampledAtTheSameTime) {
  for (const bool inherited : {false, true}) {
    SCOPED_TRACE(inherited);
    const std::string prefix =
        inherited ? "<defs><linearGradient id='base'/><linearGradient id='g' href='#base'"
                  : "<defs><linearGradient id='g'";
    const std::string suffix =
        "><stop stop-color='red'/><stop offset='1' stop-color='blue'/></linearGradient>"
        "</defs><rect width='16' height='16' fill='url(#g)'/>";
    const std::string animations =
        "<animate href='#g' attributeName='x2' from='100%' to='8' dur='2s'/>"
        "<set href='" +
        std::string(inherited ? "#base" : "#g") +
        "' attributeName='gradientUnits' to='userSpaceOnUse' dur='2s'/>";
    auto document = instantiateSubtree(prefix + suffix + animations, parser::SVGParser::Options(),
                                       Vector2i(16, 16));
    auto expected = instantiateSubtree(prefix + " gradientUnits='userSpaceOnUse' x2='12'" + suffix,
                                       parser::SVGParser::Options(), Vector2i(16, 16));
    document.setTime(1);
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
        inherited ? "inherited-sampled-gradient-units" : "sampled-gradient-units",
        editor::tests::PixelmatchIdentityParams());
  }
}

}  // namespace
}  // namespace donner::svg
