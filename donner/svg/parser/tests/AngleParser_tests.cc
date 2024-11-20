#include "donner/svg/parser/AngleParser.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/css/parser/details/ComponentValueParser.h"
#include "donner/css/parser/details/Tokenizer.h"

namespace donner::svg::parser {

namespace {

css::ComponentValue ParseComponentValue(std::string_view str) {
  css::parser::details::Tokenizer tokenizer(str);
  const std::vector<css::ComponentValue> values = css::parser::details::parseListOfComponentValues(
      tokenizer, css::parser::details::WhitespaceHandling::Keep);
  if (values.size() != 1) {
    ADD_FAILURE() << "Expected exactly one component value, got " << values.size() << ", for '"
                  << str << "'";
    return css::ComponentValue(css::Token(css::Token::EofToken(), 0));
  }

  return values[0];
}

// Create a string suffix operator with _cv for parsing a string as a single ComponentValue.
css::ComponentValue operator""_cv(const char* str, size_t len) {
  return ParseComponentValue(std::string_view(str, len));
}

}  // namespace

TEST(AngleParserTest, ParseDegrees) {
  const css::ComponentValue component("30deg"_cv);
  EXPECT_THAT(ParseAngle(component), ParseResultIs(30.0 * MathConstants<double>::kDegToRad));
}

TEST(AngleParserTest, ParseRadians) {
  const css::ComponentValue component("2rad"_cv);
  EXPECT_THAT(ParseAngle(component), ParseResultIs(2.0));
}

TEST(AngleParserTest, ParseGradians) {
  const css::ComponentValue component("200grad"_cv);
  EXPECT_THAT(ParseAngle(component), ParseResultIs(MathConstants<double>::kPi));
}

TEST(AngleParserTest, ParseTurns) {
  const css::ComponentValue component("1turn"_cv);
  EXPECT_THAT(ParseAngle(component), ParseResultIs(MathConstants<double>::kPi * 2.0));
}

TEST(AngleParserTest, InvalidUnit) {
  const css::ComponentValue component("30foo"_cv);
  EXPECT_THAT(ParseAngle(component), ParseErrorIs("Unsupported angle unit 'foo'"));
}

TEST(AngleParserTest, BareZero) {
  css::ComponentValue component("0"_cv);
  EXPECT_THAT(ParseAngle(component, AngleParseOptions::AllowBareZero), ParseResultIs(0.0));
  EXPECT_THAT(ParseAngle(component, AngleParseOptions::AllowNumbersInDegrees), ParseResultIs(0.0));
  EXPECT_THAT(ParseAngle(component, AngleParseOptions::None), ParseErrorIs("Invalid angle"));
}

TEST(AngleParserTest, BareNumber) {
  css::ComponentValue component("30"_cv);
  EXPECT_THAT(ParseAngle(component, AngleParseOptions::AllowNumbersInDegrees),
              ParseResultIs(30.0 * MathConstants<double>::kDegToRad));

  EXPECT_THAT(ParseAngle(component, AngleParseOptions::None), ParseErrorIs("Invalid angle"));
  EXPECT_THAT(ParseAngle(component, AngleParseOptions::AllowBareZero),
              ParseErrorIs("Invalid angle"));
}

}  // namespace donner::svg::parser
