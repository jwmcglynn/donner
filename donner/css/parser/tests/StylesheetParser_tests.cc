#include "donner/css/parser/StylesheetParser.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/ParseWarningSink.h"
#include "donner/css/parser/tests/TokenTestUtils.h"
#include "donner/css/tests/SelectorTestUtils.h"

using testing::ElementsAre;

namespace donner::css::parser {

MATCHER_P2(SelectorRuleIs, selector, declarations, "") {
  return testing::ExplainMatchResult(selector, arg.selector, result_listener) &&
         testing::ExplainMatchResult(declarations, arg.declarations, result_listener);
}

TEST(StylesheetParser, Empty) {
  ParseWarningSink disabled = ParseWarningSink::Disabled();
  EXPECT_THAT(StylesheetParser::Parse("", disabled).rules(), ElementsAre());
}

TEST(StylesheetParser, WithRules) {
  ParseWarningSink disabled = ParseWarningSink::Disabled();
  EXPECT_THAT(StylesheetParser::Parse(R"(
    test, .class {
      name: value;
    }
  )", disabled)
                  .rules(),
              ElementsAre(SelectorRuleIs(
                  SelectorsAre(ComplexSelectorIs(EntryIs(TypeSelectorIs("test"))),
                               ComplexSelectorIs(EntryIs(ClassSelectorIs("class")))),
                  ElementsAre(DeclarationIs("name", ElementsAre(TokenIsIdent("value")))))));
}

TEST(StylesheetParser, FontFace) {
  ParseWarningSink disabled = ParseWarningSink::Disabled();
  Stylesheet sheet = StylesheetParser::Parse(R"(
    @font-face {
      font-family: test;
      src: url(test.woff);
    }
    svg { fill: red; }
  )", disabled);

  ASSERT_EQ(sheet.fontFaces().size(), 1u);
  EXPECT_EQ(sheet.fontFaces()[0].familyName, "test");
  ASSERT_EQ(sheet.fontFaces()[0].sources.size(), 1u);
  EXPECT_EQ(sheet.fontFaces()[0].sources[0].kind, FontFaceSource::Kind::Url);
}

TEST(StylesheetParser, FontFaceDataUrl) {
  ParseWarningSink disabled = ParseWarningSink::Disabled();
  Stylesheet sheet = StylesheetParser::Parse(R"(
    @font-face {
      font-family: datafont;
      src: url(data:font/woff;base64,dGVzdA==);
    }
  )", disabled);

  ASSERT_EQ(sheet.fontFaces().size(), 1u);
  EXPECT_EQ(sheet.fontFaces()[0].familyName, "datafont");
  ASSERT_EQ(sheet.fontFaces()[0].sources.size(), 1u);
  EXPECT_EQ(sheet.fontFaces()[0].sources[0].kind, FontFaceSource::Kind::Data);
  const auto& dataPtr =
      std::get<std::shared_ptr<const std::vector<uint8_t>>>(sheet.fontFaces()[0].sources[0].payload);
  EXPECT_THAT(*dataPtr, testing::ElementsAre('t', 'e', 's', 't'));
}

}  // namespace donner::css::parser
