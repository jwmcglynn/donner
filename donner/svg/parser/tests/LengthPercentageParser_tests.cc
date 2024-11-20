#include "donner/svg/parser/LengthPercentageParser.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/css/parser/details/ComponentValueParser.h"
#include "donner/css/parser/details/Tokenizer.h"

namespace donner::svg::parser {

namespace {

const bool kAllowUserUnits = true;
const bool kNoUserUnits = false;

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

// Parse a string into a list of component values.
std::vector<css::ComponentValue> operator""_cv_list(const char* str, size_t len) {
  css::parser::details::Tokenizer tokenizer(std::string_view(str, len));
  return css::parser::details::parseListOfComponentValues(
      tokenizer, css::parser::details::WhitespaceHandling::Keep);
}

}  // namespace

TEST(LengthPercentageParserTest, ParseLength) {
  css::ComponentValue component("10px"_cv);
  EXPECT_THAT(ParseLengthPercentage(component, kNoUserUnits),
              ParseResultIs(Lengthd(10.0, Lengthd::Unit::Px)));
}

TEST(LengthPercentageParserTest, ParsePercentage) {
  css::ComponentValue component("50%"_cv);
  EXPECT_THAT(ParseLengthPercentage(component, kNoUserUnits),
              ParseResultIs(Lengthd(50.0, Lengthd::Unit::Percent)));
}

TEST(LengthPercentageParserTest, InvalidUnit) {
  css::ComponentValue component("10foo"_cv);
  EXPECT_THAT(ParseLengthPercentage(component, kAllowUserUnits),
              ParseErrorIs("Invalid unit on length"));
}

TEST(LengthPercentageParserTest, UnitlessZero) {
  // Unitless zero is always allowed.
  css::ComponentValue component("0"_cv);
  EXPECT_THAT(ParseLengthPercentage(component, kAllowUserUnits),
              ParseResultIs(Lengthd(0.0, Lengthd::Unit::None)));

  EXPECT_THAT(ParseLengthPercentage(component, kNoUserUnits),
              ParseResultIs(Lengthd(0.0, Lengthd::Unit::None)));
}

TEST(LengthPercentageParserTest, UserUnits) {
  css::ComponentValue component("10"_cv);
  EXPECT_THAT(ParseLengthPercentage(component, kAllowUserUnits),
              ParseResultIs(Lengthd(10.0, Lengthd::Unit::None)));

  EXPECT_THAT(ParseLengthPercentage(component, kNoUserUnits),
              ParseErrorIs("Invalid length or percentage"));
}

TEST(LengthPercentageParserTest, ZeroComponents) {
  {
    const ParseResult<Lengthd> result = ParseLengthPercentage(""_cv_list, kAllowUserUnits);
    EXPECT_THAT(result, ParseErrorIs("Unexpected end of input"));
    EXPECT_THAT(result, ParseErrorEndOfString());
  }

  {
    const ParseResult<Lengthd> result =
        ParseLengthPercentage("/* comment */"_cv_list, kAllowUserUnits);
    EXPECT_THAT(result, ParseErrorIs("Unexpected end of input"));
    EXPECT_THAT(result, ParseErrorEndOfString());
  }
}

TEST(LengthPercentageParserTest, MultipleComponents) {
  {
    const ParseResult<Lengthd> result = ParseLengthPercentage("10% 20%"_cv_list, kAllowUserUnits);
    EXPECT_THAT(result, ParseErrorIs("Unexpected token when parsing length or percentage"));
    EXPECT_THAT(result, ParseErrorPos(0, 3));
  }

  {
    const ParseResult<Lengthd> result =
        ParseLengthPercentage("ident 10px"_cv_list, kAllowUserUnits);
    EXPECT_THAT(result, ParseErrorIs("Unexpected token when parsing length or percentage"));
    EXPECT_THAT(result, ParseErrorPos(0, 5));
  }
}

}  // namespace donner::svg::parser
