#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/parser/tests/ParseResultTestUtils.h"
#include "donner/css/details/AnbValue.h"
#include "donner/css/parser/AnbMicrosyntaxParser.h"
#include "donner/css/parser/details/Subparsers.h"

using testing::ElementsAre;
using testing::Eq;
using testing::Optional;

namespace donner::css::parser {

using namespace donner::base::parser;  // NOLINT: For tests

namespace {

std::vector<css::ComponentValue> toComponents(std::string_view str) {
  css::details::Tokenizer tokenizer(str);
  return css::details::parseListOfComponentValues(tokenizer,
                                                  css::details::WhitespaceHandling::Keep);
}

MATCHER_P2(AnbValueIs, a, b, "") {
  return arg.value.a == a && arg.value.b == b;
}

MATCHER(NoComponentsRemaining, "") {
  return arg.remainingComponents.empty();
}

MATCHER_P(NumRemainingTokens, num, "") {
  return arg.remainingComponents.size() == num;
}

}  // namespace

using testing::AllOf;

TEST(AnbMicrosyntaxParser, Simple) {
  EXPECT_THAT(AnbMicrosyntaxParser::Parse({}),
              ParseErrorIs("An+B microsyntax expected, found empty list"));

  EXPECT_THAT(AnbMicrosyntaxParser::Parse(std::initializer_list<ComponentValue>{
                  ComponentValue(Token(Token::Ident("even"), 0))  //
              }),
              ParseResultIs(AllOf(AnbValueIs(2, 0), NoComponentsRemaining())));

  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("even")),
              ParseResultIs(AllOf(AnbValueIs(2, 0), NoComponentsRemaining())));
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("odd")),
              ParseResultIs(AllOf(AnbValueIs(2, 1), NoComponentsRemaining())));
}

/**
 * Examples from the An+B microsyntax spec: https://www.w3.org/TR/css-syntax-3/#anb-microsyntax
 */
TEST(AnbMicrosyntaxParser, ExamplesFromSpec) {
  // Example 4
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("2n+0")),
              ParseResultIs(AllOf(AnbValueIs(2, 0), NoComponentsRemaining())));
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("even")),
              ParseResultIs(AllOf(AnbValueIs(2, 0), NoComponentsRemaining())));
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("4n+1")),
              ParseResultIs(AllOf(AnbValueIs(4, 1), NoComponentsRemaining())));

  // Example 5
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("-1n+6")),
              ParseResultIs(AllOf(AnbValueIs(-1, 6), NoComponentsRemaining())));
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("-4n+10")),
              ParseResultIs(AllOf(AnbValueIs(-4, 10), NoComponentsRemaining())));

  // Example 6
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("0n+5")),
              ParseResultIs(AllOf(AnbValueIs(0, 5), NoComponentsRemaining())));

  // Example 7
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("1n+0")),
              ParseResultIs(AllOf(AnbValueIs(1, 0), NoComponentsRemaining())));
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("n+0")),
              ParseResultIs(AllOf(AnbValueIs(1, 0), NoComponentsRemaining())));
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("n")),
              ParseResultIs(AllOf(AnbValueIs(1, 0), NoComponentsRemaining())));

  // Example 8
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("2n+0")),
              ParseResultIs(AllOf(AnbValueIs(2, 0), NoComponentsRemaining())));
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("2n")),
              ParseResultIs(AllOf(AnbValueIs(2, 0), NoComponentsRemaining())));

  // Example 9
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("3n-6")),
              ParseResultIs(AllOf(AnbValueIs(3, -6), NoComponentsRemaining())));
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("3n + -6")),
              ParseErrorIs("Unexpected token when parsing An+B microsyntax"));

  // Example 10
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("3n + 1")),
              ParseResultIs(AllOf(AnbValueIs(3, 1), NoComponentsRemaining())));
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("+3n - 2")),
              ParseResultIs(AllOf(AnbValueIs(3, -2), NoComponentsRemaining())));
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("-n+ 6")),
              ParseResultIs(AllOf(AnbValueIs(-1, 6), NoComponentsRemaining())));
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("+6")),
              ParseResultIs(AllOf(AnbValueIs(0, 6), NoComponentsRemaining())));

  // Invalid whitespace
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("3 n")),
              ParseResultIs(AllOf(AnbValueIs(0, 3), NumRemainingTokens(1))))
      << "Last 'n' is not parsed";
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("+ 2n")),
              ParseErrorIs("Unexpected token when parsing An+B microsyntax"));
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("+ 2")),
              ParseErrorIs("Unexpected token when parsing An+B microsyntax"));
}

TEST(AnbMicrosyntaxParser, DigitParsing) {
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("3n-6234")),
              ParseResultIs(AllOf(AnbValueIs(3, -6234), NoComponentsRemaining())));
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("3n-6a")),
              ParseErrorIs("Unexpected token when parsing An+B microsyntax"));
}

TEST(AnbMicrosyntaxParser, SpecialTokens) {
  // Starting with '-n', which parses as an <ident-token>
  // '-n' <signed-integer>
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("-n")),
              ParseResultIs(AllOf(AnbValueIs(-1, 0), NoComponentsRemaining())));
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("-n- 2")),
              ParseResultIs(AllOf(AnbValueIs(-1, -2), NoComponentsRemaining())));
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("-n+2")),
              ParseResultIs(AllOf(AnbValueIs(-1, 2), NoComponentsRemaining())));

  // -n ['+' | '-'] <signless-integer>
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("-n + 3")),
              ParseResultIs(AllOf(AnbValueIs(-1, 3), NoComponentsRemaining())));
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("-n - 3")),
              ParseResultIs(AllOf(AnbValueIs(-1, -3), NoComponentsRemaining())));

  // Failure mode: '-n' and any other token, or unexpected EOF.
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("-n n")),
              ParseErrorIs("Unexpected token when parsing An+B microsyntax"));
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("-n +")),
              ParseErrorIs("An+B microsyntax unexpected end of list"));

  // 'n-' followed by a digit parses as an <ident-token> with embedded numbers
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("n-2")),
              ParseResultIs(AllOf(AnbValueIs(1, -2), NoComponentsRemaining())));
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("+n-2")),
              ParseResultIs(AllOf(AnbValueIs(1, -2), NoComponentsRemaining())));

  // '-n-' <signless-integer>, needs a space to be parsed as two tokens
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("-n- 123")),
              ParseResultIs(AllOf(AnbValueIs(-1, -123), NoComponentsRemaining())));

  // Failure mode: Not followed by a <signless-integer>
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("-n- +123")),
              ParseErrorIs("Unexpected token when parsing An+B microsyntax"));

  // Starting with '-n-' followed by a digit, parses as an <ident-token> with embedded numbers
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("-n-2")),
              ParseResultIs(AllOf(AnbValueIs(-1, -2), NoComponentsRemaining())));

  // '+'? n <signed-integer>
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("n + 123")),
              ParseResultIs(AllOf(AnbValueIs(1, 123), NoComponentsRemaining())));
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("+n + 123")),
              ParseResultIs(AllOf(AnbValueIs(1, 123), NoComponentsRemaining())));

  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("n - 123")),
              ParseResultIs(AllOf(AnbValueIs(1, -123), NoComponentsRemaining())));
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("+n - 123")),
              ParseResultIs(AllOf(AnbValueIs(1, -123), NoComponentsRemaining())));

  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("n+0\x10")),
              ParseResultIs(AllOf(AnbValueIs(1, 0), NumRemainingTokens(1))));

  // Failure mode: Not followed by an integer, '+' or '-'
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("+n n")),
              ParseErrorIs("Unexpected token when parsing An+B microsyntax"));

  // Failure mode: Invalid token after '+' or '-'
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("+n + n")),
              ParseErrorIs("Unexpected token when parsing An+B microsyntax"));

  // '+'? n- <signless-integer>
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("n- 2")),
              ParseResultIs(AllOf(AnbValueIs(1, -2), NoComponentsRemaining())));
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("+n- 2")),
              ParseResultIs(AllOf(AnbValueIs(1, -2), NoComponentsRemaining())));

  // Failure mode: Not a signless integer
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("+n- +2")),
              ParseErrorIs("Unexpected token when parsing An+B microsyntax"));

  // Failure mode: Unexpected end of string
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("n-")),
              ParseErrorIs("An+B microsyntax unexpected end of list"));
}

TEST(AnbMicrosyntaxParser, UnexpectedEndOfStream) {
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("+")),
              ParseErrorIs("An+B microsyntax unexpected end of list"));
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("++")),
              ParseErrorIs("An+B microsyntax unexpected end of list"));
}

TEST(AnbMicrosyntaxParser, DimensionTokens) {
  // <n-dimension> <signed-integer>
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("123n -2")),
              ParseResultIs(AllOf(AnbValueIs(123, -2), NoComponentsRemaining())));
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("123n +2")),
              ParseResultIs(AllOf(AnbValueIs(123, 2), NoComponentsRemaining())));

  // <n-dimension> ['+' | '-'] <signless-integer>
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("123n - 3")),
              ParseResultIs(AllOf(AnbValueIs(123, -3), NoComponentsRemaining())));
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("123n + 3")),
              ParseResultIs(AllOf(AnbValueIs(123, 3), NoComponentsRemaining())));

  // <n-dimension> failure
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("123n 1")),
              ParseErrorIs("Unexpected token when parsing An+B microsyntax"));

  // <ndashdigit-dimension>
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("123n-1")),
              ParseResultIs(AllOf(AnbValueIs(123, -1), NoComponentsRemaining())));
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("+123n-1")),
              ParseResultIs(AllOf(AnbValueIs(123, -1), NoComponentsRemaining())));

  // With a space parses as two tokens but has the same value
  // <ndash-dimension> <signless-integer>
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("123n- 1")),
              ParseResultIs(AllOf(AnbValueIs(123, -1), NoComponentsRemaining())));
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("+123n- 1")),
              ParseResultIs(AllOf(AnbValueIs(123, -1), NoComponentsRemaining())));

  // Failure mode: Not a signless integer
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("+123n- +2")),
              ParseErrorIs("Unexpected token when parsing An+B microsyntax"));
}

TEST(AnbMicrosyntaxParser, FunctionTokenInvalid) {
  EXPECT_THAT(AnbMicrosyntaxParser::Parse(toComponents("func()")),
              ParseErrorIs("Expected CSS token when parsing An+B microsyntax"));
}

}  // namespace donner::css::parser
