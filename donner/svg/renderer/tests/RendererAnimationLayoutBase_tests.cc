#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/renderer/tests/RendererTestUtils.h"
#include "donner/svg/tests/ParserTestUtils.h"

namespace donner::svg {
namespace {

TEST(RendererAnimationLayoutBase, ToOnlyUsesMarkerAndMaskDefaults) {
  struct Case {
    const char* name;
    const char* prefix;
    const char* suffix;
    const char* attribute;
    const char* to;
    const char* midpoint;
  };
  for (const auto item : {
           Case{"marker-default", "<defs><marker id='p' markerUnits='userSpaceOnUse'",
                "><rect width='20' height='8'/></marker></defs>"
                "<path d='M2 8H8' stroke='black' marker-end='url(#p)'/>",
                "markerWidth", "9", "6"},
           Case{"mask-default", "<defs><mask id='p'",
                "><rect width='16' height='16' fill='white'/></mask></defs>"
                "<rect x='4' y='4' width='8' height='8' mask='url(#p)'/>",
                "width", "0%", "60%"},
       }) {
    SCOPED_TRACE(item.name);
    const std::string base = std::string(item.prefix) + item.suffix;
    const std::string animation = "<animate href='#p' attributeName='" +
                                  std::string(item.attribute) + "' to='" + item.to + "' dur='2s'/>";
    auto document =
        instantiateSubtree(base + animation, parser::SVGParser::Options(), Vector2i(16, 16));
    auto expected = instantiateSubtree(
        std::string(item.prefix) + " " + item.attribute + "='" + item.midpoint + "'" + item.suffix,
        parser::SVGParser::Options(), Vector2i(16, 16));
    document.setTime(1);
    editor::tests::CompareBitmapToBitmap(RenderDocumentWithActiveBackend(document),
                                         RenderDocumentWithActiveBackend(expected), item.name,
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererAnimationLayoutBase, SymbolReferencePointAnimatesAndRestores) {
  const std::string suffix =
      "><rect width='8' height='16'/></symbol></defs>"
      "<use href='#p' x='8' width='16' height='16'/>";
  const std::string prefix = "<defs><symbol id='p' viewBox='0 0 16 16'";
  auto document = instantiateSubtree(
      prefix + suffix + "<animate href='#p' attributeName='refX' to='8' dur='2s'/>",
      parser::SVGParser::Options(), Vector2i(16, 16));
  auto expected = instantiateSubtree(prefix + " refX='4'" + suffix, parser::SVGParser::Options(),
                                     Vector2i(16, 16));
  auto base = instantiateSubtree(prefix + suffix, parser::SVGParser::Options(), Vector2i(16, 16));
  document.setTime(1);
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "symbol-reference", editor::tests::PixelmatchIdentityParams());
  document.setTime(3);
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(base),
      "symbol-reference-restored", editor::tests::PixelmatchIdentityParams());
}

}  // namespace
}  // namespace donner::svg
