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

TEST(StylesheetParser, FontFaceDescriptorsAndMultipleSources) {
  Stylesheet sheet = StylesheetParser::Parse(R"(
    @font-face {
      font-family: Multi;
      font-style: italic;
      font-weight: 700;
      font-stretch: 75%;
      font-display: swap;
      src: local("Public Sans"), url(font.woff2) format("woff2"),
           url(data:font/woff;base64,dGVzdA==) tech(color-COLRv1);
    }
  )");

  ASSERT_EQ(sheet.fontFaces().size(), 1u);
  const FontFace& face = sheet.fontFaces()[0];
  EXPECT_EQ(face.familyName, "Multi");
  ASSERT_TRUE(face.style.has_value());
  EXPECT_EQ(face.style.value(), "italic");
  ASSERT_TRUE(face.weight.has_value());
  EXPECT_EQ(face.weight.value(), "700");
  ASSERT_TRUE(face.stretch.has_value());
  EXPECT_EQ(face.stretch.value(), "75%");
  ASSERT_TRUE(face.display.has_value());
  EXPECT_EQ(face.display.value(), "swap");

  ASSERT_EQ(face.sources.size(), 3u);
  EXPECT_EQ(face.sources[0].kind, FontFaceSource::Kind::Local);
  EXPECT_EQ(std::get<RcString>(face.sources[0].payload), "Public Sans");
  EXPECT_EQ(face.sources[1].kind, FontFaceSource::Kind::Url);
  EXPECT_EQ(face.sources[1].formatHint, "woff2");
  EXPECT_EQ(face.sources[2].kind, FontFaceSource::Kind::Data);
  ASSERT_EQ(face.sources[2].techHints.size(), 1u);
  EXPECT_EQ(face.sources[2].techHints[0], "color-COLRv1");
  EXPECT_THAT(std::get<std::vector<uint8_t>>(face.sources[2].payload),
              testing::ElementsAre('t', 'e', 's', 't'));
}

}  // namespace donner::css::parser
