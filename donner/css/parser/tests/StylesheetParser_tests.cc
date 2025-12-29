#include "donner/css/parser/StylesheetParser.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/css/ColorProfile.h"
#include "donner/css/parser/tests/TokenTestUtils.h"
#include "donner/css/tests/SelectorTestUtils.h"

using testing::ElementsAre;
using testing::Optional;

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
  EXPECT_EQ(sheet.fontFaces()[0].familyName, "test");
  ASSERT_EQ(sheet.fontFaces()[0].sources.size(), 1u);
  EXPECT_EQ(sheet.fontFaces()[0].sources[0].kind, FontFaceSource::Kind::Url);
}

TEST(StylesheetParser, FontFaceDataUrl) {
  Stylesheet sheet = StylesheetParser::Parse(R"(
    @font-face {
      font-family: datafont;
      src: url(data:font/woff;base64,dGVzdA==);
    }
  )");

  ASSERT_EQ(sheet.fontFaces().size(), 1u);
  EXPECT_EQ(sheet.fontFaces()[0].familyName, "datafont");
  ASSERT_EQ(sheet.fontFaces()[0].sources.size(), 1u);
  EXPECT_EQ(sheet.fontFaces()[0].sources[0].kind, FontFaceSource::Kind::Data);
  EXPECT_THAT(std::get<std::vector<uint8_t>>(sheet.fontFaces()[0].sources[0].payload),
              testing::ElementsAre('t', 'e', 's', 't'));
}

TEST(StylesheetParser, ColorProfiles) {
  Stylesheet sheet = StylesheetParser::Parse(R"(
    @color-profile --brand { src: display-p3; }
    @color-profile --hdr { src: color(rec2020 0 0 0); }
    svg { fill: red; }
  )");

  EXPECT_THAT(sheet.colorProfiles().resolve("--brand"), Optional(ColorSpaceId::DisplayP3));
  EXPECT_THAT(sheet.colorProfiles().resolve("--hdr"), Optional(ColorSpaceId::Rec2020));
}

}  // namespace donner::css::parser
