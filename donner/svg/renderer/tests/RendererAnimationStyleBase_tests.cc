#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/renderer/tests/RendererTestUtils.h"
#include "donner/svg/tests/ParserTestUtils.h"

namespace donner::svg {
namespace {

#ifdef DONNER_TEXT_ENABLED
TEST(RendererAnimationStyleBase, FontSizeUsesTheSampledParentFontContext) {
  for (const bool animatedParent : {false, true}) {
    for (const std::string_view from : {"200%", "2em"}) {
      SCOPED_TRACE(animatedParent);
      SCOPED_TRACE(from);
      const std::string source =
          "<g id='parent' font-size='12'><text id='label' x='0' y='30' font-size='24'>H</text></g>";
      auto document = instantiateSubtree(
          source + "<animate href='#label' attributeName='font-size' from='" + std::string(from) +
              "' to='12px' dur='2s'/>" +
              (animatedParent ? "<animate href='#parent' attributeName='font-size' from='12' "
                                "to='20' dur='2s'/>"
                              : ""),
          parser::SVGParser::Options(), Vector2i(96, 48));
      auto expected = instantiateSubtree(std::string("<text x='0' y='30' font-size='") +
                                             (animatedParent ? "22" : "18") + "'>H</text>",
                                         parser::SVGParser::Options(), Vector2i(96, 48));
      document.setTime(1);
      editor::tests::CompareBitmapToBitmap(
          RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
          "font-size-parent-context", editor::tests::PixelmatchIdentityParams());
      auto restored = instantiateSubtree(source, parser::SVGParser::Options(), Vector2i(96, 48));
      document.setTime(3);
      editor::tests::CompareBitmapToBitmap(
          RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(restored),
          "font-size-parent-context-restored", editor::tests::PixelmatchIdentityParams());
    }
  }
}
#endif

TEST(RendererAnimationStyleBase, ToOnlyPaintKeepsStylesheetReferenceAndFallback) {
  for (const std::string_view paint : {"url('#g')", "url('#quoted\"g')", "url('#missing') red"}) {
    SCOPED_TRACE(paint);
    const std::string base =
        "<defs><linearGradient id='g'><stop stop-color='red'/><stop offset='1' "
        "stop-color='blue'/></linearGradient><linearGradient id='quoted\"g' href='#g'/></defs>"
        "<style>#r { fill:" +
        std::string(paint) + "; }</style><rect id='r' width='16' height='16'/>";
    auto document =
        instantiateSubtree(base + "<animate href='#r' attributeName='fill' to='green' dur='2s'/>",
                           parser::SVGParser::Options(), Vector2i(16, 16));
    auto expected = instantiateSubtree(base, parser::SVGParser::Options(), Vector2i(16, 16));
    document.setTime(0.5);
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
        "stylesheet-paint-base", editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererAnimationStyleBase, ToOnlyDashListUsesStylesheetValue) {
  auto document = instantiateSubtree(
      "<style>#p {stroke-dasharray:2 2;}</style><path id='p' d='M0 8H16' stroke='black'>"
      "<animate attributeName='stroke-dasharray' to='6 2' dur='2s'/></path>",
      parser::SVGParser::Options(), Vector2i(16, 16));
  auto expected = instantiateSubtree("<path d='M0 8H16' stroke='black' stroke-dasharray='4 2'/>",
                                     parser::SVGParser::Options(), Vector2i(16, 16));
  document.setTime(1);
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "stylesheet-dash-base", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationStyleBase, NoneDashListRemainsDiscrete) {
  auto document = instantiateSubtree(
      "<path d='M0 8H16' stroke='black'>"
      "<animate attributeName='stroke-dasharray' to='4' dur='2s'/></path>",
      parser::SVGParser::Options(), Vector2i(16, 16));
  auto expected = instantiateSubtree("<path d='M0 8H16' stroke='black'/>",
                                     parser::SVGParser::Options(), Vector2i(16, 16));
  document.setTime(0.5);
  editor::tests::CompareBitmapToBitmap(RenderDocumentWithActiveBackend(document),
                                       RenderDocumentWithActiveBackend(expected), "none-dash-base",
                                       editor::tests::PixelmatchIdentityParams());
}

}  // namespace
}  // namespace donner::svg
