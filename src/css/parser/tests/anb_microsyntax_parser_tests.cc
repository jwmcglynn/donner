#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/base/parser/tests/parse_result_test_utils.h"
#include "src/css/parser/anb_microsyntax_parser.h"
#include "src/css/parser/details/subparsers.h"

using testing::ElementsAre;
using testing::Eq;
using testing::Optional;

namespace donner::css {

namespace {

std::vector<css::ComponentValue> toComponents(std::string_view str) {
  css::details::Tokenizer tokenizer(str);
  return css::details::parseListOfComponentValues(tokenizer,
                                                  css::details::WhitespaceHandling::Keep);
}

MATCHER_P2(AnbValueIs, a, b, "") {
  return arg.value.a == a && arg.value.b == b;
}

}  // namespace

TEST(AnbMicrosyntaxParser, Simple) {
  EXPECT_THAT(AnbMicrosyntaxParser::Parse({}),
              ParseErrorIs("An+B microsyntax expected, found empty list"));

  EXPECT_THAT(AnbMicrosyntaxParser::Parse(std::initializer_list<ComponentValue>{
                  ComponentValue(Token(Token::Ident("even"), 0))  //
              }),
              ParseResultIs(AnbValueIs(2, 0)));

  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("even")), ParseResultIs(AnbValueIs(2, 0)));
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("odd")), ParseResultIs(AnbValueIs(2, 1)));
}

TEST(AnbMicrosyntaxParser, ExamplesFromSpec) {
  // Example 4
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("2n+0")), ParseResultIs(AnbValueIs(2, 0)));
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("even")), ParseResultIs(AnbValueIs(2, 0)));
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("4n+1")), ParseResultIs(AnbValueIs(4, 1)));

  // Example 5
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("-1n+6")), ParseResultIs(AnbValueIs(-1, 6)));
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("-4n+10")),
              ParseResultIs(AnbValueIs(-4, 10)));

  // Example 6
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("0n+5")), ParseResultIs(AnbValueIs(0, 5)));

  // Example 7
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("1n+0")), ParseResultIs(AnbValueIs(1, 0)));
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("n+0")), ParseResultIs(AnbValueIs(1, 0)));
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("n")), ParseResultIs(AnbValueIs(1, 0)));

  // Example 8
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("2n+0")), ParseResultIs(AnbValueIs(2, 0)));
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("2n")), ParseResultIs(AnbValueIs(2, 0)));

  // Example 9
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("3n-6")), ParseResultIs(AnbValueIs(3, -6)));
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("3n + -6")),
              ParseErrorIs("Unexpected token when parsing An+B microsyntax"));

  // Example 10
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("3n + 1")), ParseResultIs(AnbValueIs(3, 1)));
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("+3n - 2")),
              ParseResultIs(AnbValueIs(3, -2)));
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("-n+ 6")), ParseResultIs(AnbValueIs(-1, 6)));
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("+6")), ParseResultIs(AnbValueIs(0, 6)));
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("3 n")),
              ParseErrorIs("Unexpected token when parsing An+B microsyntax"));
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("+ 2n")),
              ParseErrorIs("Unexpected token when parsing An+B microsyntax"));
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("+ 2")),
              ParseErrorIs("Unexpected token when parsing An+B microsyntax"));
}

TEST(AnbMicrosyntaxParser, DigitParsing) {
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("3n-6234")),
              ParseResultIs(AnbValueIs(3, -6234)));
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("3n-6a")),
              ParseErrorIs("Unexpected token when parsing An+B microsyntax"));
}

// TODO: Add more tests

}  // namespace donner::css
