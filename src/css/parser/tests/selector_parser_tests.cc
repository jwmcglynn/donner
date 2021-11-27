#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/base/parser/tests/parse_result_test_utils.h"
#include "src/css/parser/selector_parser.h"
#include "src/css/parser/tests/selector_test_utils.h"
#include "src/css/parser/tests/token_test_utils.h"

using testing::AllOf;
using testing::ElementsAre;

namespace donner {
namespace css {

// TODO: Replace this with actual value matchers.
MATCHER_P(ToStringIs, expected, "") {
  return testing::PrintToString(arg) == expected;
}

TEST(SelectorParser, Empty) {
  EXPECT_THAT(SelectorParser::Parse(""),
              AllOf(ParseErrorPos(0, 0), ParseErrorIs("No selectors found")));
  EXPECT_THAT(SelectorParser::Parse(" \t "),
              AllOf(ParseErrorPos(0, 3), ParseErrorIs("No selectors found")));
}

TEST(SelectorParser, Simple) {
  EXPECT_THAT(SelectorParser::Parse("test"),
              ParseResultIs(ComplexSelectorIs(EntryIs(TypeSelectorIs("test")))));
  EXPECT_THAT(SelectorParser::Parse(".class-test"),
              ParseResultIs(ComplexSelectorIs(EntryIs(ClassSelectorIs("class-test")))));
  EXPECT_THAT(SelectorParser::Parse("#hash-test"),
              ParseResultIs(ComplexSelectorIs(EntryIs(IdSelectorIs("hash-test")))));

  // Using a `\` to escape cancels out the special meaning, see
  // https://www.w3.org/TR/2018/WD-selectors-4-20181121/#case-sensitive.
  EXPECT_THAT(SelectorParser::Parse("#foo\\>a"),
              ParseResultIs(ComplexSelectorIs(EntryIs(IdSelectorIs("foo>a")))));
}

TEST(SelectorParser, Multiple) {
  EXPECT_THAT(SelectorParser::Parse("test, .class"),
              ParseResultIs(SelectorsAre(ComplexSelectorIs(EntryIs(TypeSelectorIs("test"))),
                                         ComplexSelectorIs(EntryIs(ClassSelectorIs("class"))))));

  EXPECT_THAT(SelectorParser::Parse("test, .class invalid|"),
              ParseErrorIs("Expected ident after namespace prefix when parsing name"));
}

TEST(SelectorParser, CombinatorTypes) {
  EXPECT_THAT(
      SelectorParser::Parse("one two"),
      ParseResultIs(ComplexSelectorIs(EntryIs(TypeSelectorIs("one")),
                                      EntryIs(Combinator::Descendant, TypeSelectorIs("two")))));
  EXPECT_THAT(SelectorParser::Parse("one > two"),
              ParseResultIs(ComplexSelectorIs(EntryIs(TypeSelectorIs("one")),
                                              EntryIs(Combinator::Child, TypeSelectorIs("two")))));
  EXPECT_THAT(
      SelectorParser::Parse("one + two"),
      ParseResultIs(ComplexSelectorIs(EntryIs(TypeSelectorIs("one")),
                                      EntryIs(Combinator::NextSibling, TypeSelectorIs("two")))));
  EXPECT_THAT(SelectorParser::Parse("one ~ two"),
              ParseResultIs(ComplexSelectorIs(
                  EntryIs(TypeSelectorIs("one")),
                  EntryIs(Combinator::SubsequentSibling, TypeSelectorIs("two")))));
  EXPECT_THAT(SelectorParser::Parse("one || two"),
              ParseResultIs(ComplexSelectorIs(EntryIs(TypeSelectorIs("one")),
                                              EntryIs(Combinator::Column, TypeSelectorIs("two")))));
}

TEST(SelectorParser, TypeSelector) {
  EXPECT_THAT(SelectorParser::Parse("name"),
              ParseResultIs(ComplexSelectorIs(EntryIs(TypeSelectorIs("name")))));
  EXPECT_THAT(SelectorParser::Parse("ns|name"),
              ParseResultIs(ComplexSelectorIs(EntryIs(TypeSelectorIs("ns", "name")))));
  EXPECT_THAT(SelectorParser::Parse("*|name"),
              ParseResultIs(ComplexSelectorIs(EntryIs(TypeSelectorIs("*", "name")))));
  EXPECT_THAT(SelectorParser::Parse("|name"),
              ParseResultIs(ComplexSelectorIs(EntryIs(TypeSelectorIs("name")))));

  // Putting the name as a wildcard is invalid for a <wq-name>, but valid for a TypeSelector.
  EXPECT_THAT(SelectorParser::Parse("ns|*"),
              ParseResultIs(ComplexSelectorIs(EntryIs(TypeSelectorIs("ns", "*")))));

  // Invalid WqNames with a namespace but no name.
  EXPECT_THAT(SelectorParser::Parse("*|"),
              ParseErrorIs("Expected ident after namespace prefix when parsing name"));
  EXPECT_THAT(SelectorParser::Parse("first *|"),
              ParseErrorIs("Expected ident after namespace prefix when parsing name"));
  EXPECT_THAT(SelectorParser::Parse("ns|"),
              ParseErrorIs("Expected ident after namespace prefix when parsing name"));
  EXPECT_THAT(SelectorParser::Parse("first ns|"),
              ParseErrorIs("Expected ident after namespace prefix when parsing name"));
}

TEST(SelectorParser, TypeSelector_ToString) {
  EXPECT_THAT(
      SelectorParser::Parse("name"),
      ParseResultIs(ToStringIs("Selector(ComplexSelector(CompoundSelector(TypeSelector(name))))")));

  EXPECT_THAT(SelectorParser::Parse("ns|name"),
              ParseResultIs(ToStringIs(
                  "Selector(ComplexSelector(CompoundSelector(TypeSelector(ns|name))))")));
  EXPECT_THAT(SelectorParser::Parse("*|name"),
              ParseResultIs(
                  ToStringIs("Selector(ComplexSelector(CompoundSelector(TypeSelector(*|name))))")));
  EXPECT_THAT(
      SelectorParser::Parse("|name"),
      ParseResultIs(ToStringIs("Selector(ComplexSelector(CompoundSelector(TypeSelector(name))))")));
  EXPECT_THAT(
      SelectorParser::Parse("ns|*"),
      ParseResultIs(ToStringIs("Selector(ComplexSelector(CompoundSelector(TypeSelector(ns|*))))")));
}

TEST(SelectorParser, PseudoElementSelector) {
  EXPECT_THAT(SelectorParser::Parse("::after"),
              ParseResultIs(ComplexSelectorIs(EntryIs(PseudoElementSelectorIs("after")))));
  EXPECT_THAT(
      SelectorParser::Parse("::after()"),
      ParseResultIs(ComplexSelectorIs(EntryIs(PseudoElementSelectorIs("after", ElementsAre())))));

  EXPECT_THAT(SelectorParser::Parse("::after(one two)"),
              ParseResultIs(ComplexSelectorIs(EntryIs(PseudoElementSelectorIs(
                  "after", ElementsAre(TokenIsIdent("one"), TokenIsWhitespace(" "),
                                       TokenIsIdent("two")))))));
}

TEST(SelectorParser, PseudoElementSelector_ToString) {
  EXPECT_THAT(SelectorParser::Parse("::after"),
              ParseResultIs(ToStringIs(
                  "Selector(ComplexSelector(CompoundSelector(PseudoElementSelector(after))))")));
  EXPECT_THAT(
      SelectorParser::Parse("::after()"),
      ParseResultIs(ToStringIs(
          "Selector(ComplexSelector(CompoundSelector(PseudoElementSelector(after args[]))))")));

  EXPECT_THAT(
      SelectorParser::Parse("::after(one two)"),
      ParseResultIs(ToStringIs("Selector(ComplexSelector(CompoundSelector(PseudoElementSelector("
                               "after args[Token { Ident(one) offset: 8 }, Token { Whitespace(' ', "
                               "len=1) offset: 11 }, Token { Ident(two) offset: 12 }, ]))))")));
}

TEST(SelectorParser, PseudoClassSelector) {
  EXPECT_THAT(SelectorParser::Parse(":after"),
              ParseResultIs(ComplexSelectorIs(EntryIs(PseudoClassSelectorIs("after")))));
  EXPECT_THAT(
      SelectorParser::Parse(":after()"),
      ParseResultIs(ComplexSelectorIs(EntryIs(PseudoClassSelectorIs("after", ElementsAre())))));

  EXPECT_THAT(SelectorParser::Parse(":after(one two)"),
              ParseResultIs(ComplexSelectorIs(EntryIs(PseudoClassSelectorIs(
                  "after", ElementsAre(TokenIsIdent("one"), TokenIsWhitespace(" "),
                                       TokenIsIdent("two")))))));
}

TEST(SelectorParser, PseudoClassSelector_ToString) {
  EXPECT_THAT(SelectorParser::Parse(":after"),
              ParseResultIs(ToStringIs(
                  "Selector(ComplexSelector(CompoundSelector(PseudoClassSelector(after))))")));
  EXPECT_THAT(
      SelectorParser::Parse(":after()"),
      ParseResultIs(ToStringIs(
          "Selector(ComplexSelector(CompoundSelector(PseudoClassSelector(after args[]))))")));

  EXPECT_THAT(
      SelectorParser::Parse(":after(one two)"),
      ParseResultIs(ToStringIs("Selector(ComplexSelector(CompoundSelector(PseudoClassSelector("
                               "after args[Token { Ident(one) offset: 7 }, Token { Whitespace(' ', "
                               "len=1) offset: 10 }, Token { Ident(two) offset: 11 }, ]))))")));
}

TEST(SelectorParser, AttrMatcher) {
  EXPECT_THAT(
      SelectorParser::Parse("a[test]"),
      ParseResultIs(ComplexSelectorIs(EntryIs(TypeSelectorIs("a"), AttributeSelectorIs("test")))));
}

TEST(SelectorParser, AttrMatcher_ToString) {
  EXPECT_THAT(SelectorParser::Parse("a[test]"),
              ParseResultIs(ToStringIs("Selector(ComplexSelector(CompoundSelector(TypeSelector(a), "
                                       "AttributeSelector(test))))")));
}

// view-source:http://test.csswg.org/suites/selectors-4_dev/nightly-unstable/html/is.htm
TEST(SelectorParser, CssTestSuite_Is) {
  // Simple selector arguments
  EXPECT_THAT(SelectorParser::Parse(".a :is(.b, .c)"),
              ParseResultIs(ComplexSelectorIs(
                  EntryIs(ClassSelectorIs("a")),
                  EntryIs(Combinator::Descendant,
                          PseudoClassSelectorIs(
                              "is", ElementsAre(TokenIsDelim('.'), TokenIsIdent("b"),
                                                TokenIsComma(), TokenIsWhitespace(" "),
                                                TokenIsDelim('.'), TokenIsIdent("c")))))));

  // Compound selector arguments
  EXPECT_THAT(SelectorParser::Parse(".a :is(.c#d, .e)"),
              ParseResultIs(ComplexSelectorIs(
                  EntryIs(ClassSelectorIs("a")),
                  EntryIs(Combinator::Descendant,
                          PseudoClassSelectorIs(
                              "is", ElementsAre(TokenIsDelim('.'), TokenIsIdent("c"),
                                                TokenIsHash(Token::Hash::Type::Id, "d"),
                                                TokenIsComma(), TokenIsWhitespace(" "),
                                                TokenIsDelim('.'), TokenIsIdent("e")))))));

  // Complex selector arguments
  EXPECT_THAT(SelectorParser::Parse(".a .g>.b"),
              ParseResultIs(ComplexSelectorIs(EntryIs(ClassSelectorIs("a")),
                                              EntryIs(Combinator::Descendant, ClassSelectorIs("g")),
                                              EntryIs(Combinator::Child, ClassSelectorIs("b")))));

  EXPECT_THAT(
      SelectorParser::Parse(".a :is(.e+.f, .g>.b, .h)"),
      ParseResultIs(ComplexSelectorIs(
          EntryIs(ClassSelectorIs("a")),
          EntryIs(Combinator::Descendant,
                  PseudoClassSelectorIs(
                      "is", ElementsAre(TokenIsDelim('.'), TokenIsIdent("e"), TokenIsDelim('+'),
                                        TokenIsDelim('.'), TokenIsIdent("f"), TokenIsComma(),
                                        TokenIsWhitespace(" "), TokenIsDelim('.'),
                                        TokenIsIdent("g"), TokenIsDelim('>'), TokenIsDelim('.'),
                                        TokenIsIdent("b"), TokenIsComma(), TokenIsWhitespace(" "),
                                        TokenIsDelim('.'), TokenIsIdent("h")))))));
  EXPECT_THAT(SelectorParser::Parse(".g>.b"),
              ParseResultIs(ComplexSelectorIs(EntryIs(ClassSelectorIs("g")),
                                              EntryIs(Combinator::Child, ClassSelectorIs("b")))));
  EXPECT_THAT(
      SelectorParser::Parse(".a .h"),
      ParseResultIs(ComplexSelectorIs(EntryIs(ClassSelectorIs("a")),
                                      EntryIs(Combinator::Descendant, ClassSelectorIs("h")))));

  // Nested
  EXPECT_THAT(
      SelectorParser::Parse(".a+.c>.e"),
      ParseResultIs(ComplexSelectorIs(EntryIs(ClassSelectorIs("a")),
                                      EntryIs(Combinator::NextSibling, ClassSelectorIs("c")),
                                      EntryIs(Combinator::Child, ClassSelectorIs("e")))));
  EXPECT_THAT(SelectorParser::Parse(".c>.a+.e"),
              ParseResultIs(ComplexSelectorIs(
                  EntryIs(ClassSelectorIs("c")), EntryIs(Combinator::Child, ClassSelectorIs("a")),
                  EntryIs(Combinator::NextSibling, ClassSelectorIs("e")))));
  EXPECT_THAT(
      SelectorParser::Parse(".a+:is(.b+.f, :is(.c>.e, .g))"),
      ParseResultIs(ComplexSelectorIs(
          EntryIs(ClassSelectorIs("a")),
          EntryIs(
              Combinator::NextSibling,
              PseudoClassSelectorIs(
                  "is",
                  ElementsAre(
                      TokenIsDelim('.'), TokenIsIdent("b"), TokenIsDelim('+'), TokenIsDelim('.'),
                      TokenIsIdent("f"), TokenIsComma(), TokenIsWhitespace(" "), TokenIsColon(),
                      FunctionIs(
                          "is", ElementsAre(TokenIsDelim('.'), TokenIsIdent("c"), TokenIsDelim('>'),
                                            TokenIsDelim('.'), TokenIsIdent("e"), TokenIsComma(),
                                            TokenIsWhitespace(" "), TokenIsDelim('.'),
                                            TokenIsIdent("g")))))))));
  EXPECT_THAT(SelectorParser::Parse(".c>.e"),
              ParseResultIs(ComplexSelectorIs(EntryIs(ClassSelectorIs("c")),
                                              EntryIs(Combinator::Child, ClassSelectorIs("e")))));
}

// TODO: Add more tests from
// http://test.csswg.org/suites/selectors-4_dev/nightly-unstable/html/toc.htm

}  // namespace css
}  // namespace donner
