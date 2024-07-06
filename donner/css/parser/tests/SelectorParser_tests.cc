#include "donner/css/parser/SelectorParser.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/parser/tests/ParseResultTestUtils.h"
#include "donner/base/tests/BaseTestUtils.h"
#include "donner/css/ComponentValue.h"
#include "donner/css/Token.h"
#include "donner/css/parser/details/Subparsers.h"
#include "donner/css/parser/tests/TokenTestUtils.h"
#include "donner/css/tests/SelectorTestUtils.h"

using testing::AllOf;
using testing::ElementsAre;

namespace donner::css::parser {

using namespace base::parser;  // NOLINT: For tests

namespace {

std::vector<ComponentValue> TokenizeString(std::string_view str) {
  details::Tokenizer tokenizer_(str);
  return details::parseListOfComponentValues(tokenizer_);
}

}  // namespace

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
  // https://www.w3.org/TR/selectors-4/#case-sensitive.
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

  EXPECT_THAT(SelectorParser::Parse("a[test=insensitive s]"),
              ParseResultIs(ComplexSelectorIs(
                  EntryIs(TypeSelectorIs("a"),
                          AttributeSelectorIs("test", MatcherIs(AttrMatcher::Eq, "insensitive",
                                                                MatcherOptions::CaseSensitive))))));
  EXPECT_THAT(SelectorParser::Parse("a[test=\"value\"s]"),
              ParseResultIs(ComplexSelectorIs(
                  EntryIs(TypeSelectorIs("a"),
                          AttributeSelectorIs("test", MatcherIs(AttrMatcher::Eq, "value",
                                                                MatcherOptions::CaseSensitive))))));

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

TEST(SelectorParser, ForgivingSelectorList) {
  // Test case 1: All valid selectors
  EXPECT_THAT(SelectorParser::ParseForgivingSelectorList(TokenizeString("div, .class, #id")),
              SelectorsAre(ComplexSelectorIs(EntryIs(TypeSelectorIs("div"))),
                           ComplexSelectorIs(EntryIs(ClassSelectorIs("class"))),
                           ComplexSelectorIs(EntryIs(IdSelectorIs("id")))));

  // Test case 2: Mixed valid and invalid selectors
  EXPECT_THAT(SelectorParser::ParseForgivingSelectorList(TokenizeString("div, ::-invalid, .class")),
              SelectorsAre(ComplexSelectorIs(EntryIs(TypeSelectorIs("div"))),
                           ComplexSelectorIs(EntryIs(ClassSelectorIs("class")))));

  // Test case 3: All invalid selectors
  EXPECT_THAT(SelectorParser::ParseForgivingSelectorList(TokenizeString("div:, :invalid, 1234")),
              SelectorsAre()  // Empty result
  );

  // Test case 4: Complex selectors with some invalid parts
  EXPECT_THAT(SelectorParser::ParseForgivingSelectorList(
                  TokenizeString("div > p, a[href]:not(:visited), span::before:invalid")),
              SelectorsAre(ComplexSelectorIs(EntryIs(TypeSelectorIs("div")),
                                             EntryIs(Combinator::Child, TypeSelectorIs("p")))));

  // Test case 5: Whitespace handling
  EXPECT_THAT(
      SelectorParser::ParseForgivingSelectorList(TokenizeString("  div  ,  .class  ,  #id  ")),
      SelectorsAre(ComplexSelectorIs(EntryIs(TypeSelectorIs("div"))),
                   ComplexSelectorIs(EntryIs(ClassSelectorIs("class"))),
                   ComplexSelectorIs(EntryIs(IdSelectorIs("id")))));

  // Test case 6: Empty input
  EXPECT_THAT(SelectorParser::ParseForgivingSelectorList(TokenizeString("")),
              SelectorsAre()  // Empty result
  );

  // Test case 7: Single invalid selector
  EXPECT_THAT(SelectorParser::ParseForgivingSelectorList(TokenizeString("div:")),
              SelectorsAre()  // Empty result
  );

  // Test case 8: Pseudo-elements and pseudo-classes
  EXPECT_THAT(SelectorParser::ParseForgivingSelectorList(
                  TokenizeString("a:hover, p::first-line, div:nth-child(2n+1)")),
              SelectorsAre(ComplexSelectorIs(EntryIs(
                  TypeSelectorIs("div"),
                  PseudoClassSelectorIs("nth-child", ElementsAre(testing::_, testing::_))))));

  // Test case 9: Attribute selectors
  EXPECT_THAT(
      SelectorParser::ParseForgivingSelectorList(
          TokenizeString("a[href], img[src^='https'], input[type='text']")),
      SelectorsAre(ComplexSelectorIs(EntryIs(TypeSelectorIs("a"), AttributeSelectorIs("href"))),
                   ComplexSelectorIs(EntryIs(
                       TypeSelectorIs("img"),
                       AttributeSelectorIs("src", MatcherIs(AttrMatcher::PrefixMatch, "https")))),
                   ComplexSelectorIs(
                       EntryIs(TypeSelectorIs("input"),
                               AttributeSelectorIs("type", MatcherIs(AttrMatcher::Eq, "text"))))));

  // Test case 10: Combinators
  EXPECT_THAT(
      SelectorParser::ParseForgivingSelectorList(TokenizeString("div > p, ul + ol, h1 ~ h2")),
      SelectorsAre(
          ComplexSelectorIs(EntryIs(TypeSelectorIs("div")),
                            EntryIs(Combinator::Child, TypeSelectorIs("p"))),
          ComplexSelectorIs(EntryIs(TypeSelectorIs("ul")),
                            EntryIs(Combinator::NextSibling, TypeSelectorIs("ol"))),
          ComplexSelectorIs(EntryIs(TypeSelectorIs("h1")),
                            EntryIs(Combinator::SubsequentSibling, TypeSelectorIs("h2")))));
}

TEST(SelectorParsing, ForgivingRelativeSelectorList) {
  // Regular list
  EXPECT_THAT(
      SelectorParser::ParseForgivingRelativeSelectorList(TokenizeString("div, .class, #id")),
      SelectorsAre(ComplexSelectorIs(EntryIs(TypeSelectorIs("div"))),
                   ComplexSelectorIs(EntryIs(ClassSelectorIs("class"))),
                   ComplexSelectorIs(EntryIs(IdSelectorIs("id")))));

  // Beginning with a combinator
  EXPECT_THAT(SelectorParser::ParseForgivingRelativeSelectorList(TokenizeString("> div")),
              SelectorsAre(ComplexSelectorIs(EntryIs(Combinator::Child, TypeSelectorIs("div")))));
  EXPECT_THAT(SelectorParser::ParseForgivingRelativeSelectorList(TokenizeString("  >div")),
              SelectorsAre(ComplexSelectorIs(EntryIs(Combinator::Child, TypeSelectorIs("div")))));

  // List with combinators
  EXPECT_THAT(SelectorParser::ParseForgivingRelativeSelectorList(TokenizeString("> p, + ol, ~ h2")),
              SelectorsAre(
                  ComplexSelectorIs(EntryIs(Combinator::Child, TypeSelectorIs("p"))),
                  ComplexSelectorIs(EntryIs(Combinator::NextSibling, TypeSelectorIs("ol"))),
                  ComplexSelectorIs(EntryIs(Combinator::SubsequentSibling, TypeSelectorIs("h2")))));
}

// TODO: Add more tests from
// http://test.csswg.org/suites/selectors-4_dev/nightly-unstable/html/toc.htm

}  // namespace donner::css::parser
