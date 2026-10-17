#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/renderer/tests/RendererTestUtils.h"
#include "donner/svg/tests/ParserTestUtils.h"

namespace donner::svg {
namespace {

#ifdef DONNER_TEXT_ENABLED
SVGDocument ParseTextPosition(std::string_view source) {
  return instantiateSubtree(source, parser::SVGParser::Options(), Vector2i(160, 48));
}

void CompareTextPosition(SVGDocument& document, std::string_view expectedSource,
                         const std::string& checkpoint) {
  const std::string source(document.source());
  auto expected = ParseTextPosition(expectedSource);
  editor::tests::CompareBitmapToBitmap(RenderDocumentWithActiveBackend(document),
                                       RenderDocumentWithActiveBackend(expected), checkpoint,
                                       editor::tests::PixelmatchIdentityParams());
  EXPECT_EQ(document.source(), source);
}

TEST(RendererAnimationTextPosition, InheritedListsUseAddressableCharacterOffsets) {
  struct Case {
    const char* name;
    const char* parent;
    const char* before;
    const char* content;
    const char* after;
    const char* attribute;
    const char* to;
    const char* midpoint;
  };
  const Case cases[] = {
      {"preceding-character", "dx='0 4'", "A", "B", "", "dx", "8", "6"},
      {"two-dx-values", "dx='0 4 8'", "A", "BC", "", "dx", "8 12", "6 10"},
      {"two-dy-values", "dy='0 2 4'", "A", "BC", "", "dy", "6 8", "4 6"},
      {"nested-anchor", "dx='0 4 8'", "A<a>B", "C", "</a>", "dx", "12", "10"},
      {"repeat-last-rotation", "rotate='0 30'", "A", "BC", "", "rotate", "90 150", "60 90"},
      {"multibyte-character", "dx='0 4'", "&#xE9;", "H", "", "dx", "8", "6"},
      {"collapsed-whitespace", "dx='0 2 4'", "A   ", "B", "", "dx", "8", "6"},
      {"split-owner", "dx='0 2 4 6'", "A", "B<a>C</a>D", "", "dx", "6 8 10", "4 6 8"},
  };
  for (const auto& item : cases) {
    SCOPED_TRACE(item.name);
    const std::string prefix =
        std::string("<text y='24' font-size='12' ") + item.parent + ">" + item.before;
    const std::string suffix =
        std::string(">") + item.content + "</tspan>" + item.after + "</text>";
    auto document = ParseTextPosition(prefix + "<tspan id='target'" + suffix +
                                      "<animate href='#target' attributeName='" + item.attribute +
                                      "' to='" + item.to + "' dur='2s'/>");
    for (const double time : {0.0, 1.0, 3.0, 1.0}) {
      SCOPED_TRACE(time);
      document.setTime(time);
      const std::string sampled =
          time == 1 ? std::string(" ") + item.attribute + "='" + item.midpoint + "'" : "";
      CompareTextPosition(document, prefix + "<tspan" + sampled + suffix,
                          std::string("inherited-text-") + item.name + "-" + std::to_string(time));
    }
  }
}

TEST(RendererAnimationTextPosition, ParentListEditsRemainTheBaseAfterAnimationRemoval) {
  auto document = ParseTextPosition(
      "<text id='root' y='24' font-size='12' dx='0 4'>A<tspan id='target'>B</tspan></text>"
      "<animate id='animation' href='#target' attributeName='dx' to='12' dur='2s'/>");
  document.setTime(1);
  CompareTextPosition(document, "<text y='24' font-size='12'>A<tspan dx='8'>B</tspan></text>",
                      "text-list-before-edit");
  auto root = document.querySelector("#root");
  ASSERT_TRUE(root.has_value());
  root->setAttribute("dx", "0 8");
  document.setTime(1);
  CompareTextPosition(document, "<text y='24' font-size='12'>A<tspan dx='10'>B</tspan></text>",
                      "text-list-after-edit");
  auto animation = document.querySelector("#animation");
  ASSERT_TRUE(animation.has_value());
  ASSERT_TRUE(document.removeElement(*animation).applied);
  CompareTextPosition(document, "<text y='24' font-size='12'>A<tspan dx='8'>B</tspan></text>",
                      "text-list-after-removal");
}

TEST(RendererAnimationTextPosition, InheritedRelativeListsUseEachTextInstanceFont) {
  auto document = ParseTextPosition(
      "<defs><text id='label' y='24' dx='0 1em'>A<tspan id='target'>B</tspan></text></defs>"
      "<animate href='#target' attributeName='dx' to='20px' dur='2s'/>"
      "<use href='#label' font-size='8'/><use href='#label' x='60' font-size='16'/>");
  for (const double time : {1.0, 3.0, 1.0}) {
    SCOPED_TRACE(time);
    document.setTime(time);
    const std::string first = time == 1 ? "14" : "8";
    const std::string second = time == 1 ? "18" : "16";
    CompareTextPosition(document,
                        "<text y='24' font-size='8'>A<tspan dx='" + first +
                            "'>B</tspan></text><g transform='translate(60)'>"
                            "<text y='24' font-size='16'>A<tspan dx='" +
                            second + "'>B</tspan></text></g>",
                        "text-instance-relative-list-" + std::to_string(time));
  }
}
#endif

}  // namespace
}  // namespace donner::svg
