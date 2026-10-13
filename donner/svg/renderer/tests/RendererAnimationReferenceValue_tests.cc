#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/renderer/tests/RendererTestUtils.h"
#include "donner/svg/tests/ParserTestUtils.h"

namespace donner::svg {
namespace {

TEST(RendererAnimationReferenceValue, EmptyReferenceClearsAndRestoresUse) {
  const std::string content =
      "<defs><rect id='r' width='16' height='16'/></defs><use id='u' href='#r'/>";
  for (const std::string_view animation : {
           "<set href='#u' attributeName='href' to='' dur='2s'/>",
           "<animate href='#u' attributeName='href' to='' dur='2s'/>",
       }) {
    SCOPED_TRACE(animation);
    auto document = instantiateSubtree(content + std::string(animation),
                                       parser::SVGParser::Options(), Vector2i(16, 16));
    auto empty = instantiateSubtree("<rect width='0' height='0'/>", parser::SVGParser::Options(),
                                    Vector2i(16, 16));
    auto base = instantiateSubtree(content, parser::SVGParser::Options(), Vector2i(16, 16));
    document.setTime(1.5);
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(empty),
        "empty-use-reference", editor::tests::PixelmatchIdentityParams());
    document.setTime(3);
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(base),
        "restored-use-reference", editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererAnimationReferenceValue, MissingOrInvalidReferenceAnimationKeepsBase) {
  const std::string content =
      "<defs><rect id='r' width='16' height='16'/></defs><use id='u' href='#r'/>";
  for (const std::string_view animation : {
           "<set href='#u' attributeName='href' dur='2s'/>",
           "<animate href='#u' attributeName='href' by='#other' dur='2s'/>",
           "<animate href='#u' attributeName='href' to='' keyTimes='0;2' dur='2s'/>",
       }) {
    SCOPED_TRACE(animation);
    auto document = instantiateSubtree(content + std::string(animation),
                                       parser::SVGParser::Options(), Vector2i(16, 16));
    auto base = instantiateSubtree(content, parser::SVGParser::Options(), Vector2i(16, 16));
    document.setTime(1.5);
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(base),
        "invalid-reference-preserves-base", editor::tests::PixelmatchIdentityParams());
  }
}

}  // namespace
}  // namespace donner::svg
