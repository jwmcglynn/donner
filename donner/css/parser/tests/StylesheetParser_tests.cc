#include "donner/css/parser/StylesheetParser.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/css/parser/tests/TokenTestUtils.h"
#include "donner/css/tests/SelectorTestUtils.h"

using testing::ElementsAre;

namespace donner::css::parser {

MATCHER_P2(SelectorRuleIs, selector, declarations, "") {
  return testing::ExplainMatchResult(selector, arg.selector, result_listener) &&
         testing::ExplainMatchResult(declarations, arg.declarations, result_listener);
}

TEST(StylesheetParser, Empty) {
  EXPECT_THAT(StylesheetParser::Parse("").rules(), ElementsAre());
}

TEST(StylesheetParser, WithRules) {
  EXPECT_THAT(StylesheetParser::Parse(R"(
    test, .class {
      name: value;
    }
  )")
                  .rules(),
              ElementsAre(SelectorRuleIs(
                  SelectorsAre(ComplexSelectorIs(EntryIs(TypeSelectorIs("test"))),
                               ComplexSelectorIs(EntryIs(ClassSelectorIs("class")))),
                  ElementsAre(DeclarationIs("name", ElementsAre(TokenIsIdent("value")))))));
}

TEST(StylesheetParser, FontFace) {
  Stylesheet sheet = StylesheetParser::Parse(R"(
    @font-face {
      font-family: test;
      src: url(test.woff);
    }
    svg { fill: red; }
  )");

  ASSERT_EQ(sheet.fontFaces().size(), 1u);
  EXPECT_EQ(sheet.fontFaces()[0].familyName_, "test");
  ASSERT_EQ(sheet.fontFaces()[0].sources_.size(), 1u);
  EXPECT_EQ(sheet.fontFaces()[0].sources_[0].kind, FontFaceSource::Kind::Url);
}

TEST(StylesheetParser, FontFaceDataUrl) {
  Stylesheet sheet = StylesheetParser::Parse(R"(
    @font-face {
      font-family: datafont;
      src: url(data:font/woff;base64,dGVzdA==);
    }
  )");

  ASSERT_EQ(sheet.fontFaces().size(), 1u);
  EXPECT_EQ(sheet.fontFaces()[0].familyName_, "datafont");
  ASSERT_EQ(sheet.fontFaces()[0].sources_.size(), 1u);
  EXPECT_EQ(sheet.fontFaces()[0].sources_[0].kind, FontFaceSource::Kind::Data);
  EXPECT_THAT(std::get<std::vector<uint8_t>>(sheet.fontFaces()[0].sources_[0].payload),
              testing::ElementsAre('t', 'e', 's', 't'));
}

}  // namespace donner::css::parser
