#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <tuple>

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

// A `<use>` or nested `<svg>` position is layout state, and `<use>` content is a shadow copy of
// its referenced element. When an animation stops applying, whether it ended or was restarted,
// every instance must return to its authored position.
TEST(RendererAnimationLayoutBase, AnimatedPositionsRestoreAfterEndAndRestart) {
  struct Case {
    const char* name;
    std::string_view beforeAnimation;
    std::string_view afterAnimation;
  };
  for (const auto item : {
           Case{"use", "<defs><rect id='t' width='4' height='16'/></defs><use href='#t' x='2'>",
                "</use>"},
           Case{"nested-svg",
                "<defs><rect id='t' width='4' height='16'/></defs><svg x='2'><use href='#t'/>",
                "</svg>"},
           Case{"use-shadow-content", "<defs><rect id='t' x='2' width='4' height='16'>",
                "</rect></defs><use href='#t'/>"},
       }) {
    SCOPED_TRACE(item.name);
    const auto withAnimation = [&](std::string_view animation) {
      return std::string(item.beforeAnimation) + std::string(animation) +
             std::string(item.afterAnimation);
    };
    auto document =
        instantiateSubtree(withAnimation("<set attributeName='x' to='10' begin='1s' dur='2s'/>"),
                           parser::SVGParser::Options(), Vector2i(16, 16));
    auto moved = instantiateSubtree(withAnimation("<set attributeName='x' to='10'/>"),
                                    parser::SVGParser::Options(), Vector2i(16, 16));
    auto base =
        instantiateSubtree(withAnimation(""), parser::SVGParser::Options(), Vector2i(16, 16));
    const auto baseBitmap = RenderDocumentWithActiveBackend(base);

    document.setTime(2);
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(moved),
        std::string(item.name) + "-active", editor::tests::PixelmatchIdentityParams());
    document.setTime(4);
    editor::tests::CompareBitmapToBitmap(RenderDocumentWithActiveBackend(document), baseBitmap,
                                         std::string(item.name) + "-ended",
                                         editor::tests::PixelmatchIdentityParams());

    document.setTime(2);
    std::ignore = RenderDocumentWithActiveBackend(document);
    document.restartAnimations();
    editor::tests::CompareBitmapToBitmap(RenderDocumentWithActiveBackend(document), baseBitmap,
                                         std::string(item.name) + "-restarted",
                                         editor::tests::PixelmatchIdentityParams());
  }
}

}  // namespace
}  // namespace donner::svg
