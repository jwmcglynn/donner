#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/base/parser/tests/parse_result_test_utils.h"
#include "src/css/parser/selector_parser.h"
#include "src/css/parser/tests/token_test_utils.h"

using testing::AllOf;

namespace donner {
namespace css {

// TODO: Replace this with actual value matchers.
MATCHER_P(ToStringIs, expected, "") {
  return testing::PrintToString(arg) == expected;
}

TEST(SelectorParser, Empty) {
  EXPECT_THAT(SelectorParser::Parse(""),
              AllOf(ParseErrorPos(0, 0), ParseErrorIs("No selectors found")));
}

TEST(SelectorParser, Simple) {
  EXPECT_THAT(SelectorParser::Parse("test"),
              ParseResultIs(ToStringIs("Selector(ComplexSelector(TypeSelector(test)))")));
  EXPECT_THAT(SelectorParser::Parse(".class-test"),
              ParseResultIs(ToStringIs("Selector(ComplexSelector(ClassSelector(class-test)))")));
  EXPECT_THAT(SelectorParser::Parse("#hash-test"),
              ParseResultIs(ToStringIs("Selector(ComplexSelector(IdSelector(hash-test)))")));

  // Using a `\` to escape cancels out the special meaning, see
  // https://www.w3.org/TR/2018/WD-selectors-4-20181121/#case-sensitive.
  EXPECT_THAT(SelectorParser::Parse("#foo\\>a"),
              ParseResultIs(ToStringIs("Selector(ComplexSelector(IdSelector(foo>a)))")));
}

TEST(SelectorParser, CombinatorTypes) {
  EXPECT_THAT(SelectorParser::Parse("one two"),
              ParseResultIs(ToStringIs(
                  "Selector(ComplexSelector(TypeSelector(one) ' ' TypeSelector(two)))")));
  EXPECT_THAT(SelectorParser::Parse("one > two"),
              ParseResultIs(ToStringIs(
                  "Selector(ComplexSelector(TypeSelector(one) '>' TypeSelector(two)))")));
  EXPECT_THAT(SelectorParser::Parse("one + two"),
              ParseResultIs(ToStringIs(
                  "Selector(ComplexSelector(TypeSelector(one) '+' TypeSelector(two)))")));
  EXPECT_THAT(SelectorParser::Parse("one ~ two"),
              ParseResultIs(ToStringIs(
                  "Selector(ComplexSelector(TypeSelector(one) '~' TypeSelector(two)))")));
  EXPECT_THAT(SelectorParser::Parse("one || two"),
              ParseResultIs(ToStringIs(
                  "Selector(ComplexSelector(TypeSelector(one) '||' TypeSelector(two)))")));
}

// view-source:http://test.csswg.org/suites/selectors-4_dev/nightly-unstable/html/is.htm
TEST(SelectorParser, CssTestSuite_Is) {
  // Simple selector arguments
  EXPECT_THAT(SelectorParser::Parse(".a :is(.b, .c)"),
              ParseResultIs(ToStringIs(
                  "Selector(ComplexSelector(ClassSelector(a) ' ' PseudoClassSelector(is args[Token "
                  "{ Delim(.) offset: 7 }, Token { Ident(b) offset: 8 }, Token { Comma offset: 9 "
                  "}, Token { Whitespace(' ', len=1) offset: 10 }, Token { Delim(.) offset: 11 }, "
                  "Token { Ident(c) offset: 12 }, ])))")));

  // Compound selector arguments
  EXPECT_THAT(SelectorParser::Parse(".a :is(.c#d, .e)"),
              ParseResultIs(ToStringIs(
                  "Selector(ComplexSelector(ClassSelector(a) ' ' PseudoClassSelector(is args[Token "
                  "{ Delim(.) offset: 7 }, Token { Ident(c) offset: 8 }, Token { Hash(id: d) "
                  "offset: 9 }, Token { Comma offset: 11 }, Token { Whitespace(' ', len=1) offset: "
                  "12 }, Token { Delim(.) offset: 13 }, Token { Ident(e) offset: 14 }, ])))")));

  // Complex selector arguments
  EXPECT_THAT(SelectorParser::Parse(".a .g>.b"),
              ParseResultIs(ToStringIs("Selector(ComplexSelector(ClassSelector(a) ' ' "
                                       "ClassSelector(g) '>' ClassSelector(b)))")));
  EXPECT_THAT(SelectorParser::Parse(".a :is(.e+.f, .g>.b, .h)"),
              ParseResultIs(ToStringIs(
                  "Selector(ComplexSelector(ClassSelector(a) ' ' PseudoClassSelector(is args[Token "
                  "{ Delim(.) offset: 7 }, Token { Ident(e) offset: 8 }, Token { Delim(+) offset: "
                  "9 }, Token { Delim(.) offset: 10 }, Token { Ident(f) offset: 11 }, Token { "
                  "Comma offset: 12 }, Token { Whitespace(' ', len=1) offset: 13 }, Token { "
                  "Delim(.) offset: 14 }, Token { Ident(g) offset: 15 }, Token { Delim(>) offset: "
                  "16 }, Token { Delim(.) offset: 17 }, Token { Ident(b) offset: 18 }, Token { "
                  "Comma offset: 19 }, Token { Whitespace(' ', len=1) offset: 20 }, Token { "
                  "Delim(.) offset: 21 }, Token { Ident(h) offset: 22 }, ])))")));
  EXPECT_THAT(SelectorParser::Parse(".g>.b"),
              ParseResultIs(
                  ToStringIs("Selector(ComplexSelector(ClassSelector(g) '>' ClassSelector(b)))")));
  EXPECT_THAT(SelectorParser::Parse(".a .h"),
              ParseResultIs(
                  ToStringIs("Selector(ComplexSelector(ClassSelector(a) ' ' ClassSelector(h)))")));

  // Nested
  EXPECT_THAT(SelectorParser::Parse(".a+.c>.e"),
              ParseResultIs(ToStringIs("Selector(ComplexSelector(ClassSelector(a) '+' "
                                       "ClassSelector(c) '>' ClassSelector(e)))")));
  EXPECT_THAT(SelectorParser::Parse(".c>.a+.e"),
              ParseResultIs(ToStringIs("Selector(ComplexSelector(ClassSelector(c) '>' "
                                       "ClassSelector(a) '+' ClassSelector(e)))")));
  EXPECT_THAT(
      SelectorParser::Parse(".a+:is(.b+.f, :is(.c>.e, .g))"),
      ParseResultIs(ToStringIs(
          "Selector(ComplexSelector(ClassSelector(a) '+' PseudoClassSelector(is args[Token { "
          "Delim(.) offset: 7 }, Token { Ident(b) offset: 8 }, Token { Delim(+) offset: 9 }, Token "
          "{ Delim(.) offset: 10 }, Token { Ident(f) offset: 11 }, Token { Comma offset: 12 }, "
          "Token { Whitespace(' ', len=1) offset: 13 }, Token { Colon offset: 14 }, Function { is( "
          "Token { Delim(.) offset: 18 } Token { Ident(c) offset: 19 } Token { Delim(>) offset: 20 "
          "} Token { Delim(.) offset: 21 } Token { Ident(e) offset: 22 } Token { Comma offset: 23 "
          "} Token { Whitespace(' ', len=1) offset: 24 } Token { Delim(.) offset: 25 } Token { "
          "Ident(g) offset: 26 } ) }, ])))")));
  EXPECT_THAT(SelectorParser::Parse(".c>.e"),
              ParseResultIs(
                  ToStringIs("Selector(ComplexSelector(ClassSelector(c) '>' ClassSelector(e)))")));
}

// TODO: Add more tests from
// http://test.csswg.org/suites/selectors-4_dev/nightly-unstable/html/toc.htm

}  // namespace css
}  // namespace donner
