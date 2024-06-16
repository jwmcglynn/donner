#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/base/parser/tests/parse_result_test_utils.h"
#include "src/base/tests/base_test_utils.h"
#include "src/css/parser/selector_parser.h"
#include "src/css/parser/tests/selector_test_utils.h"
#include "src/css/parser/tests/token_test_utils.h"

using testing::AllOf;
using testing::ElementsAre;

namespace donner::css::parser {

using namespace base::parser;  // NOLINT: For tests

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
  EXPECT_THAT(SelectorParser::Parse("*"),
              ParseResultIs(ComplexSelectorIs(EntryIs(TypeSelectorIs("*")))));
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

  EXPECT_THAT(
      SelectorParser::Parse("a|b|c"),
      ParseResultIs(ComplexSelectorIs(EntryIs(TypeSelectorIs("a", "b"), TypeSelectorIs("c")))));

  EXPECT_THAT(SelectorParser::Parse("a |b|c"),
              ParseResultIs(ComplexSelectorIs(EntryIs(TypeSelectorIs("a")),
                                              EntryIs(TypeSelectorIs("b"), TypeSelectorIs("c")))));
}

TEST(SelectorParser, TypeSelectorToString) {
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

TEST(SelectorParser, PseudoElementSelectorToString) {
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

TEST(SelectorParser, PseudoClassSelectorToString) {
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

TEST(SelectorParser, AttributeSelector) {
  EXPECT_THAT(
      SelectorParser::Parse("a[test]"),
      ParseResultIs(ComplexSelectorIs(EntryIs(TypeSelectorIs("a"), AttributeSelectorIs("test")))));
  EXPECT_THAT(
      SelectorParser::Parse("a[test=\"value\"]"),
      ParseResultIs(ComplexSelectorIs(EntryIs(
          TypeSelectorIs("a"), AttributeSelectorIs("test", MatcherIs(AttrMatcher::Eq, "value"))))));
  EXPECT_THAT(
      SelectorParser::Parse("a[test=ident]"),
      ParseResultIs(ComplexSelectorIs(EntryIs(
          TypeSelectorIs("a"), AttributeSelectorIs("test", MatcherIs(AttrMatcher::Eq, "ident"))))));

  EXPECT_THAT(SelectorParser::Parse("a[test=insensitive i]"),
              ParseResultIs(ComplexSelectorIs(EntryIs(
                  TypeSelectorIs("a"),
                  AttributeSelectorIs("test", MatcherIs(AttrMatcher::Eq, "insensitive",
                                                        MatcherOptions::CaseInsensitive))))));
  EXPECT_THAT(SelectorParser::Parse("a[test=\"value\"i]"),
              ParseResultIs(ComplexSelectorIs(EntryIs(
                  TypeSelectorIs("a"),
                  AttributeSelectorIs("test", MatcherIs(AttrMatcher::Eq, "value",
                                                        MatcherOptions::CaseInsensitive))))));

  EXPECT_THAT(SelectorParser::Parse("a[zero~=one]"),
              ParseResultIs(ComplexSelectorIs(
                  EntryIs(TypeSelectorIs("a"),
                          AttributeSelectorIs("zero", MatcherIs(AttrMatcher::Includes, "one"))))));
  EXPECT_THAT(SelectorParser::Parse("a[one|=two]"),
              ParseResultIs(ComplexSelectorIs(
                  EntryIs(TypeSelectorIs("a"),
                          AttributeSelectorIs("one", MatcherIs(AttrMatcher::DashMatch, "two"))))));
  EXPECT_THAT(SelectorParser::Parse("a[three^=four]"),
              ParseResultIs(ComplexSelectorIs(EntryIs(
                  TypeSelectorIs("a"),
                  AttributeSelectorIs("three", MatcherIs(AttrMatcher::PrefixMatch, "four"))))));
  EXPECT_THAT(SelectorParser::Parse("a[five$=six]"),
              ParseResultIs(ComplexSelectorIs(EntryIs(
                  TypeSelectorIs("a"),
                  AttributeSelectorIs("five", MatcherIs(AttrMatcher::SuffixMatch, "six"))))));
  EXPECT_THAT(SelectorParser::Parse("a[seven*=eight]"),
              ParseResultIs(ComplexSelectorIs(EntryIs(
                  TypeSelectorIs("a"),
                  AttributeSelectorIs("seven", MatcherIs(AttrMatcher::SubstringMatch, "eight"))))));

  // With whitespace.
  EXPECT_THAT(SelectorParser::Parse("a[ key |= value ]"),
              ParseResultIs(ComplexSelectorIs(EntryIs(
                  TypeSelectorIs("a"),
                  AttributeSelectorIs("key", MatcherIs(AttrMatcher::DashMatch, "value"))))));
}

TEST(SelectorParser, AttributeSelectorToString) {
  EXPECT_THAT(SelectorParser::Parse("a[test]"),
              ParseResultIs(ToStringIs("Selector(ComplexSelector(CompoundSelector(TypeSelector(a), "
                                       "AttributeSelector(test))))")));

  EXPECT_THAT(SelectorParser::Parse("a[test=\"value\"]"),
              ParseResultIs(ToStringIs("Selector(ComplexSelector(CompoundSelector(TypeSelector(a), "
                                       "AttributeSelector(test Eq(=) value))))")));
  EXPECT_THAT(SelectorParser::Parse("a[test=ident]"),
              ParseResultIs(ToStringIs("Selector(ComplexSelector(CompoundSelector(TypeSelector(a), "
                                       "AttributeSelector(test Eq(=) ident))))")));
  EXPECT_THAT(
      SelectorParser::Parse("a[test=insensitive i]"),
      ParseResultIs(ToStringIs("Selector(ComplexSelector(CompoundSelector(TypeSelector(a), "
                               "AttributeSelector(test Eq(=) insensitive (case-insensitive)))))")));
  EXPECT_THAT(
      SelectorParser::Parse("a[test=\"value\"i]"),
      ParseResultIs(ToStringIs("Selector(ComplexSelector(CompoundSelector(TypeSelector(a), "
                               "AttributeSelector(test Eq(=) value (case-insensitive)))))")));

  EXPECT_THAT(SelectorParser::Parse("a[zero~=one]"),
              ParseResultIs(ToStringIs("Selector(ComplexSelector(CompoundSelector(TypeSelector(a), "
                                       "AttributeSelector(zero Includes(~=) one))))")));
  EXPECT_THAT(SelectorParser::Parse("a[one|=two]"),
              ParseResultIs(ToStringIs("Selector(ComplexSelector(CompoundSelector(TypeSelector(a), "
                                       "AttributeSelector(one DashMatch(|=) two))))")));
  EXPECT_THAT(SelectorParser::Parse("a[three^=four]"),
              ParseResultIs(ToStringIs("Selector(ComplexSelector(CompoundSelector(TypeSelector(a), "
                                       "AttributeSelector(three PrefixMatch(^=) four))))")));
  EXPECT_THAT(SelectorParser::Parse("a[five$=six]"),
              ParseResultIs(ToStringIs("Selector(ComplexSelector(CompoundSelector(TypeSelector(a), "
                                       "AttributeSelector(five SuffixMatch($=) six))))")));
  EXPECT_THAT(SelectorParser::Parse("a[seven*=eight]"),
              ParseResultIs(ToStringIs("Selector(ComplexSelector(CompoundSelector(TypeSelector(a), "
                                       "AttributeSelector(seven SubstringMatch(*=) eight))))")));
}

TEST(SelectorParser, InvalidNsPrefix) {
  EXPECT_THAT(SelectorParser::Parse("a[*]"),
              ParseErrorIs("Expected name when parsing attribute selector"));
}

TEST(SelectorParser, InvalidWqName) {
  EXPECT_THAT(SelectorParser::Parse("5"),
              ParseErrorIs("Unexpected token when parsing compound selector"));
  EXPECT_THAT(SelectorParser::Parse("a[3]"),
              ParseErrorIs("Expected name when parsing attribute selector"));
}

TEST(SelectorParser, InvalidCompoundSelector) {
  EXPECT_THAT(SelectorParser::Parse("a/"),
              ParseErrorIs("Unexpected token when parsing compound selector"));
}

TEST(SelectorParser, InvalidCombinator) {
  EXPECT_THAT(SelectorParser::Parse("a ! b"),
              ParseErrorIs("Unexpected token when parsing compound selector"));
  EXPECT_THAT(SelectorParser::Parse("a @ b"),
              ParseErrorIs("Unexpected token when parsing compound selector"));
}

TEST(SelectorParser, InvalidClassSelector) {
  EXPECT_THAT(SelectorParser::Parse("."),
              ParseErrorIs("Expected ident when parsing class selector"));
  EXPECT_THAT(SelectorParser::Parse(".:"),
              ParseErrorIs("Expected ident when parsing class selector"));
  EXPECT_THAT(SelectorParser::Parse(".func()"),
              ParseErrorIs("Expected ident when parsing class selector"));
}

TEST(SelectorParser, InvalidPseudo) {
  EXPECT_THAT(SelectorParser::Parse("::\"invalid\""),
              ParseErrorIs("Expected ident or function after ':' for pseudo class selector"));
  EXPECT_THAT(SelectorParser::Parse(":::three"),
              ParseErrorIs("Expected ident or function after ':' for pseudo class selector"));
  EXPECT_THAT(SelectorParser::Parse(":[test]"),
              ParseErrorIs("Expected ident or function after ':' for pseudo class selector"));
}

TEST(SelectorParser, InvalidAttributeSelector) {
  EXPECT_THAT(SelectorParser::Parse("(test)"),
              ParseErrorIs("Unexpected block type, expected '[' delimeter"));
  EXPECT_THAT(SelectorParser::Parse("{test}"),
              ParseErrorIs("Unexpected block type, expected '[' delimeter"));
  EXPECT_THAT(SelectorParser::Parse("<test>"),
              ParseErrorIs("Unexpected token when parsing compound selector"));
  EXPECT_THAT(SelectorParser::Parse("a(test)"),
              ParseErrorIs("Unexpected token when parsing compound selector"));
  EXPECT_THAT(SelectorParser::Parse("a{test}"),
              ParseErrorIs("Unexpected block type, expected '[' delimeter"));
  EXPECT_THAT(SelectorParser::Parse("a<test>"),
              ParseErrorIs("Unexpected token when parsing compound selector"));

  EXPECT_THAT(
      SelectorParser::Parse("[attr*]"),
      ParseErrorIs(
          "Invalid attribute matcher, it must be either '~=', '|=', '^=', '$=', '*=', or '='"));

  EXPECT_THAT(SelectorParser::Parse("[attr~=]"),
              ParseErrorIs(
                  "Expected string or ident after matcher ('~=', '|=', '^=', '$=', '*=', or '=')"));
  EXPECT_THAT(SelectorParser::Parse("[attr~=extra[]]"),
              ParseErrorIs("Expected end of attribute selector, but found more items"));
}

TEST(SelectorParser, Specificity) {
  EXPECT_THAT(SelectorParser::Parse("test"),
              ParseResultIs(SpecificityIs(Specificity::FromABC(0, 0, 1))));
  EXPECT_THAT(SelectorParser::Parse(".test"),
              ParseResultIs(SpecificityIs(Specificity::FromABC(0, 1, 0))));
  EXPECT_THAT(SelectorParser::Parse("#test"),
              ParseResultIs(SpecificityIs(Specificity::FromABC(1, 0, 0))));
  EXPECT_THAT(SelectorParser::Parse("::after"),
              ParseResultIs(SpecificityIs(Specificity::FromABC(0, 0, 1))));
  EXPECT_THAT(SelectorParser::Parse(":after(one)"),
              ParseResultIs(SpecificityIs(Specificity::FromABC(0, 1, 0))));
  EXPECT_THAT(SelectorParser::Parse("a[attr=value]"),
              ParseResultIs(SpecificityIs(Specificity::FromABC(0, 1, 1))));

  EXPECT_THAT(SelectorParser::Parse("*"),
              ParseResultIs(SpecificityIs(Specificity::FromABC(0, 0, 0))))
      << "Universal selectors are ignored";

  EXPECT_THAT(SelectorParser::Parse("* > a#b.class::after"),
              ParseResultIs(SpecificityIs(Specificity::FromABC(1, 1, 2))));
}

// view-source:http://test.csswg.org/suites/selectors-4_dev/nightly-unstable/html/is.htm
TEST(SelectorParser, CssTestSuiteIs) {
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

}  // namespace donner::css::parser
