/**
 * Tests for ostream output operators across CSS types.
 *
 * Covers operator<< for types that don't have dedicated test files exercising their
 * string output: Combinator, AttrMatcher, Specificity::SpecialType, AnbValue, and
 * Stylesheet/SelectorRule.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <optional>
#include <sstream>
#include <type_traits>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/css/ComponentValue.h"
#include "donner/css/Rule.h"
#include "donner/css/Specificity.h"
#include "donner/css/Stylesheet.h"
#include "donner/css/Token.h"
#include "donner/css/details/AnbValue.h"
#include "donner/css/selectors/AttributeSelector.h"
#include "donner/css/selectors/ClassSelector.h"
#include "donner/css/selectors/ComplexSelector.h"
#include "donner/css/selectors/CompoundSelector.h"
#include "donner/css/selectors/IdSelector.h"
#include "donner/css/selectors/PseudoElementSelector.h"
#include "donner/css/selectors/TypeSelector.h"

namespace donner::css {

AtRule MakeAtRuleWithPreludeAndBlock() {
  AtRule rule(RcString("media"));
  rule.prelude.emplace_back(Token(Token::Ident("screen"), 0));
  rule.block.emplace(Token::indexOf<Token::CurlyBracket>(), FileOffset::Offset(14));
  rule.block->values.emplace_back(Token(Token::Ident("color"), 16));
  return rule;
}

/// @test Ostream output \c operator<< for all \ref Combinator values.
TEST(CombinatorTest, OstreamOutput) {
  EXPECT_THAT(Combinator::Descendant, ToStringIs("' '"));
  EXPECT_THAT(Combinator::Child, ToStringIs("'>'"));
  EXPECT_THAT(Combinator::NextSibling, ToStringIs("'+'"));
  EXPECT_THAT(Combinator::SubsequentSibling, ToStringIs("'~'"));
  EXPECT_THAT(Combinator::Column, ToStringIs("'||'"));
}

/// @test Ostream output \c operator<< for all \ref AttrMatcher values.
TEST(AttrMatcherTest, OstreamOutput) {
  EXPECT_THAT(AttrMatcher::Includes, ToStringIs("Includes(~=)"));
  EXPECT_THAT(AttrMatcher::DashMatch, ToStringIs("DashMatch(|=)"));
  EXPECT_THAT(AttrMatcher::PrefixMatch, ToStringIs("PrefixMatch(^=)"));
  EXPECT_THAT(AttrMatcher::SuffixMatch, ToStringIs("SuffixMatch($=)"));
  EXPECT_THAT(AttrMatcher::SubstringMatch, ToStringIs("SubstringMatch(*=)"));
  EXPECT_THAT(AttrMatcher::Eq, ToStringIs("Eq(=)"));
}

/// @test Ostream output \c operator<< for all \ref Specificity::SpecialType values.
TEST(SpecificitySpecialTypeTest, OstreamOutput) {
  EXPECT_THAT(Specificity::SpecialType::UserAgent, ToStringIs("UserAgent"));
  EXPECT_THAT(Specificity::SpecialType::None, ToStringIs("None"));
  EXPECT_THAT(Specificity::SpecialType::StyleAttribute, ToStringIs("StyleAttribute"));
  EXPECT_THAT(Specificity::SpecialType::Important, ToStringIs("Important"));
  EXPECT_THAT(Specificity::SpecialType::Override, ToStringIs("Override"));
}

/// @test Ostream output \c operator<< for \ref AnbValue.
TEST(AnbValueTest, OstreamOutput) {
  EXPECT_THAT(AnbValue(2, 3), ToStringIs("AnbValue{ 2n+3 }"));
  EXPECT_THAT(AnbValue(0, 0), ToStringIs("AnbValue{ 0n+0 }"));
  EXPECT_THAT(AnbValue(1, -1), ToStringIs("AnbValue{ 1n-1 }"));
  EXPECT_THAT(AnbValue(-3, 5), ToStringIs("AnbValue{ -3n+5 }"));
}

/// @test Ostream output \c operator<< for inline \ref Token value types.
TEST(TokenValueTest, OstreamOutput) {
  EXPECT_THAT(Token::Function("rgb"), ToStringIs("Function(rgb)"));
  EXPECT_THAT(Token::AtKeyword("media"), ToStringIs("AtKeyword(media)"));
  EXPECT_THAT(Token::Hash(Token::Hash::Type::Unrestricted, "123"),
              ToStringIs("Hash(unrestricted: 123)"));
  EXPECT_THAT(Token::Hash(Token::Hash::Type::Id, "main"), ToStringIs("Hash(id: main)"));
  EXPECT_THAT(Token::String("hello"), ToStringIs("String(\"hello\")"));
  EXPECT_THAT(Token::BadString("partial"), ToStringIs("BadString(\"partial\")"));
  EXPECT_THAT(Token::Url("image.png"), ToStringIs("Url(image.png)"));
  EXPECT_THAT(Token::BadUrl{}, ToStringIs("BadUrl"));
  EXPECT_THAT(Token::Delim('!'), ToStringIs("Delim(!)"));
  EXPECT_THAT(Token::Number(42, "42", NumberType::Integer),
              ToStringIs("Number(42, str='42', integer)"));
  EXPECT_THAT(Token::Number(1.5, "1.5", NumberType::Number),
              ToStringIs("Number(1.5, str='1.5', number)"));
  EXPECT_THAT(Token::Percentage(50, "50", NumberType::Integer),
              ToStringIs("Percentage(50, str='50', integer)"));
  EXPECT_THAT(Token::Percentage(12.5, "12.5", NumberType::Number),
              ToStringIs("Percentage(12.5, str='12.5', number)"));
  EXPECT_THAT(Token::Dimension(10, "px", Lengthd::Unit::Px, "10", NumberType::Integer),
              ToStringIs("Dimension(10px, str='10', integer)"));
  EXPECT_THAT(Token::Dimension(2.5, "em", Lengthd::Unit::Em, "2.5", NumberType::Number),
              ToStringIs("Dimension(2.5em, str='2.5', number)"));
  EXPECT_THAT(Token::Whitespace(" \n"), ToStringIs("Whitespace(' \n', len=2)"));
  EXPECT_THAT(Token::CDO{}, ToStringIs("CDO"));
  EXPECT_THAT(Token::CDC{}, ToStringIs("CDC"));
  EXPECT_THAT(Token::Colon{}, ToStringIs("Colon"));
  EXPECT_THAT(Token::Semicolon{}, ToStringIs("Semicolon"));
  EXPECT_THAT(Token::Comma{}, ToStringIs("Comma"));
  EXPECT_THAT(Token::SquareBracket{}, ToStringIs("SquareBracket"));
  EXPECT_THAT(Token::Parenthesis{}, ToStringIs("Parenthesis"));
  EXPECT_THAT(Token::CurlyBracket{}, ToStringIs("CurlyBracket"));
  EXPECT_THAT(Token::CloseSquareBracket{}, ToStringIs("CloseSquareBracket"));
  EXPECT_THAT(Token::CloseParenthesis{}, ToStringIs("CloseParenthesis"));
  EXPECT_THAT(Token::CloseCurlyBracket{}, ToStringIs("CloseCurlyBracket"));
  EXPECT_THAT(Token::ErrorToken(Token::ErrorToken::Type::EofInString),
              ToStringIs("ErrorToken(EofInString)"));
  EXPECT_THAT(Token::ErrorToken(Token::ErrorToken::Type::EofInComment),
              ToStringIs("ErrorToken(EofInComment)"));
  EXPECT_THAT(Token::ErrorToken(Token::ErrorToken::Type::EofInUrl),
              ToStringIs("ErrorToken(EofInUrl)"));
  EXPECT_THAT(Token::EofToken{}, ToStringIs("EofToken"));
}

TEST(AtRuleTest, EqualityComparesNamePreludeAndBlock) {
  const AtRule rule = MakeAtRuleWithPreludeAndBlock();
  AtRule same = MakeAtRuleWithPreludeAndBlock();
  EXPECT_EQ(rule, same);

  AtRule differentName = MakeAtRuleWithPreludeAndBlock();
  differentName.name = RcString("supports");
  EXPECT_NE(rule, differentName);

  AtRule differentPrelude = MakeAtRuleWithPreludeAndBlock();
  differentPrelude.prelude.emplace_back(Token(Token::Ident("print"), 7));
  EXPECT_NE(rule, differentPrelude);

  AtRule withoutBlock = MakeAtRuleWithPreludeAndBlock();
  withoutBlock.block.reset();
  EXPECT_NE(rule, withoutBlock);
}

TEST(AtRuleTest, OstreamOutputIncludesPreludeAndOptionalBlock) {
  AtRule withoutBlock(RcString("import"));
  withoutBlock.prelude.emplace_back(Token(Token::Url("theme.css"), 8));
  EXPECT_THAT(withoutBlock, ToStringIs(R"(AtRule {
  import
  Token { Url(theme.css) offset: FileOffset[offset 8] }
})"));

  std::ostringstream output;
  output << MakeAtRuleWithPreludeAndBlock();
  EXPECT_THAT(output.str(), testing::HasSubstr("AtRule {\n  media\n"));
  EXPECT_THAT(output.str(), testing::HasSubstr("Token { Ident(screen)"));
  EXPECT_THAT(output.str(), testing::HasSubstr("{ SimpleBlock"));
}

TEST(CssFunctionTest, EqualityComparesNameAndValues) {
  Function function(RcString("rgb"), FileOffset::Offset(0));
  function.values.emplace_back(Token(Token::Number(1.0, "1", NumberType::Integer), 4));

  Function same(RcString("rgb"), FileOffset::Offset(10));
  same.values.emplace_back(Token(Token::Number(1.0, "1", NumberType::Integer), 4));
  EXPECT_EQ(function, same);

  Function differentName(RcString("var"), FileOffset::Offset(0));
  differentName.values.emplace_back(Token(Token::Number(1.0, "1", NumberType::Integer), 4));
  EXPECT_NE(function, differentName);

  Function differentValues(RcString("rgb"), FileOffset::Offset(0));
  differentValues.values.emplace_back(Token(Token::Number(2.0, "2", NumberType::Integer), 4));
  EXPECT_NE(function, differentValues);
}

TEST(CssFunctionTest, OstreamOutputIncludesValues) {
  Function function(RcString("rgb"), FileOffset::Offset(0));
  function.values.emplace_back(Token(Token::Ident("red"), 4));

  EXPECT_THAT(function, ToStringIs("Function { rgb( Token { Ident(red) offset: "
                                   "FileOffset[offset 4] } ) }"));
}

TEST(SimpleBlockTest, EqualityComparesAssociatedTokenAndValues) {
  SimpleBlock block(Token::indexOf<Token::CurlyBracket>(), FileOffset::Offset(0));
  block.values.emplace_back(Token(Token::Ident("fill"), 1));

  SimpleBlock same(Token::indexOf<Token::CurlyBracket>(), FileOffset::Offset(2));
  same.values.emplace_back(Token(Token::Ident("fill"), 1));
  EXPECT_EQ(block, same);

  SimpleBlock differentToken(Token::indexOf<Token::SquareBracket>(), FileOffset::Offset(0));
  differentToken.values.emplace_back(Token(Token::Ident("fill"), 1));
  EXPECT_NE(block, differentToken);

  SimpleBlock differentValues(Token::indexOf<Token::CurlyBracket>(), FileOffset::Offset(0));
  differentValues.values.emplace_back(Token(Token::Ident("stroke"), 1));
  EXPECT_NE(block, differentValues);
}

TEST(SimpleBlockTest, OstreamOutputCoversAssociatedTokensAndValues) {
  SimpleBlock curly(Token::indexOf<Token::CurlyBracket>(), FileOffset::Offset(0));
  curly.values.emplace_back(Token(Token::Ident("fill"), 1));
  EXPECT_THAT(curly, ToStringIs("SimpleBlock {\n"
                                "  token='{'\n"
                                "  Token { Ident(fill) offset: FileOffset[offset 1] }\n"
                                "}"));

  EXPECT_THAT(SimpleBlock(Token::indexOf<Token::SquareBracket>(), FileOffset::Offset(0)),
              ToStringIs("SimpleBlock {\n"
                         "  token='['\n"
                         "}"));
  EXPECT_THAT(SimpleBlock(Token::indexOf<Token::Parenthesis>(), FileOffset::Offset(0)),
              ToStringIs("SimpleBlock {\n"
                         "  token='('\n"
                         "}"));
  EXPECT_THAT(SimpleBlock(TokenIndex{999}, FileOffset::Offset(0)), ToStringIs("SimpleBlock {\n"
                                                                              "  token=<unknown>\n"
                                                                              "}"));
}

TEST(SimpleBlockTest, ToCssTextWithUnknownAssociatedTokenOmitsDelimiters) {
  SimpleBlock block(TokenIndex{999}, FileOffset::Offset(0));
  block.values.emplace_back(Token(Token::Ident("fill"), 1));

  EXPECT_EQ(block.toCssText(), "fill");
}

/// @test Inline \ref Token helpers expose the active value and source offset.
TEST(TokenTest, AccessorsVisitAndOstreamOutput) {
  Token token(Token::Ident("fill"), 12);

  EXPECT_TRUE(token.is<Token::Ident>());
  EXPECT_FALSE(token.is<Token::String>());
  EXPECT_EQ(token.tokenIndex(), Token::indexOf<Token::Ident>());
  EXPECT_EQ(token.offset(), FileOffset::Offset(12));
  EXPECT_EQ(token.get<Token::Ident>().value, RcString("fill"));
  ASSERT_NE(token.tryGet<Token::Ident>(), nullptr);
  EXPECT_EQ(token.tryGet<Token::String>(), nullptr);
  EXPECT_EQ(token.visit([](const auto& value) {
    using ValueType = std::remove_cvref_t<decltype(value)>;
    if constexpr (std::is_same_v<ValueType, Token::Ident>) {
      return std::string_view(value.value).size();
    } else {
      return std::size_t{0};
    }
  }),
            4u);
  EXPECT_THAT(token, ToStringIs("Token { Ident(fill) offset: FileOffset[offset 12] }"));

  Token moved(Token::String("label"), 4);
  Token::String string = std::move(moved).get<Token::String>();
  EXPECT_EQ(string.value, RcString("label"));
}

/// @test \ref Token parse-error classification includes all parser recovery tokens.
TEST(TokenTest, ParseErrorClassification) {
  EXPECT_FALSE(Token(Token::Ident("fill"), 0).isParseError());
  EXPECT_TRUE(Token(Token::BadUrl{}, 1).isParseError());
  EXPECT_TRUE(Token(Token::BadString("partial"), 2).isParseError());
  EXPECT_TRUE(Token(Token::CloseParenthesis{}, 3).isParseError());
  EXPECT_TRUE(Token(Token::CloseSquareBracket{}, 4).isParseError());
  EXPECT_TRUE(Token(Token::CloseCurlyBracket{}, 5).isParseError());
  EXPECT_FALSE(Token(Token::ErrorToken(Token::ErrorToken::Type::EofInString), 6).isParseError());
}

/// @test \ref Token equality compares both active value and source offset.
TEST(TokenTest, EqualityIncludesValueAndOffset) {
  const Token token(Token::Ident("fill"), 12);

  EXPECT_EQ(token, Token(Token::Ident("fill"), 12));
  EXPECT_NE(token, Token(Token::Ident("stroke"), 12));
  EXPECT_NE(token, Token(Token::Ident("fill"), 13));
}

TEST(TokenValueTest, NumericEqualityComparesEveryField) {
  const Token::Number number(1.0, "1", NumberType::Integer);
  EXPECT_EQ(number, Token::Number(1.0, "1", NumberType::Integer));
  EXPECT_NE(number, Token::Number(2.0, "1", NumberType::Integer));
  EXPECT_NE(number, Token::Number(1.0, "1.0", NumberType::Integer));
  EXPECT_NE(number, Token::Number(1.0, "1", NumberType::Number));

  const Token::Percentage percentage(50.0, "50", NumberType::Integer);
  EXPECT_EQ(percentage, Token::Percentage(50.0, "50", NumberType::Integer));
  EXPECT_NE(percentage, Token::Percentage(60.0, "50", NumberType::Integer));
  EXPECT_NE(percentage, Token::Percentage(50.0, "50.0", NumberType::Integer));
  EXPECT_NE(percentage, Token::Percentage(50.0, "50", NumberType::Number));

  const Token::Dimension dimension(10.0, "px", Lengthd::Unit::Px, "10", NumberType::Integer);
  EXPECT_EQ(dimension, Token::Dimension(10.0, "px", Lengthd::Unit::Px, "10", NumberType::Integer));
  EXPECT_NE(dimension, Token::Dimension(11.0, "px", Lengthd::Unit::Px, "10", NumberType::Integer));
  EXPECT_NE(dimension, Token::Dimension(10.0, "em", Lengthd::Unit::Px, "10", NumberType::Integer));
  EXPECT_NE(dimension, Token::Dimension(10.0, "px", Lengthd::Unit::Em, "10", NumberType::Integer));
  EXPECT_NE(dimension,
            Token::Dimension(10.0, "px", Lengthd::Unit::Px, "10.0", NumberType::Integer));
  EXPECT_NE(dimension, Token::Dimension(10.0, "px", Lengthd::Unit::Px, "10", NumberType::Number));
  EXPECT_NE(dimension, Token::Dimension(10.0, "px", std::nullopt, "10", NumberType::Integer));
}

/// @test \ref Token out-of-line special members preserve token value and source offset.
TEST(TokenTest, CopyAndMoveSpecialMembersPreserveValueAndOffset) {
  const Token original(Token::String("label"), 4);

  Token copyConstructed(original);
  EXPECT_EQ(copyConstructed, original);

  Token copyAssigned(Token::Ident("other"), 99);
  copyAssigned = original;
  EXPECT_EQ(copyAssigned, original);

  Token moveConstructed(std::move(copyConstructed));
  EXPECT_EQ(moveConstructed, original);

  Token moveAssigned(Token::Ident("other"), 99);
  moveAssigned = std::move(copyAssigned);
  EXPECT_EQ(moveAssigned, original);
}

/// @test Ostream output \c operator<< for \ref ClassSelector.
TEST(ClassSelectorTest, OstreamOutput) {
  ClassSelector selector(RcString("highlight"));
  EXPECT_THAT(selector, ToStringIs("ClassSelector(highlight)"));
}

/// @test Ostream output \c operator<< for \ref IdSelector.
TEST(IdSelectorTest, OstreamOutput) {
  IdSelector selector(RcString("main-content"));
  EXPECT_THAT(selector, ToStringIs("IdSelector(main-content)"));
}

/// @test Ostream output \c operator<< for empty \ref Stylesheet.
TEST(StylesheetTest, OstreamOutputEmpty) {
  Stylesheet stylesheet;
  EXPECT_THAT(stylesheet, ToStringIs(""));
}

}  // namespace donner::css
