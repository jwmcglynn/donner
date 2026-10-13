#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/renderer/tests/RendererTestUtils.h"
#include "donner/svg/tests/ParserTestUtils.h"

namespace donner::svg {
namespace {

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
