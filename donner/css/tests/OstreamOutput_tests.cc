/**
 * Tests for ostream output operators across CSS types.
 *
 * Covers operator<< for types that don't have dedicated test files exercising their
 * string output: Combinator, AttrMatcher, Specificity::SpecialType, AnbValue, and
 * Stylesheet/SelectorRule.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <type_traits>

#include "donner/base/tests/BaseTestUtils.h"
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
