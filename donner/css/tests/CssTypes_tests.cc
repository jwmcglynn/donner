/**
 * @file Tests for CSS types: ComponentValue, ComplexSelector, PseudoClassSelector.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/FileOffset.h"
#include "donner/css/ComponentValue.h"
#include "donner/css/Selector.h"
#include "donner/css/Specificity.h"
#include "donner/css/selectors/ComplexSelector.h"
#include "donner/css/selectors/PseudoClassSelector.h"
#include "donner/css/parser/SelectorParser.h"
#include "donner/base/tests/BaseTestUtils.h"

using testing::Eq;
using testing::Ne;

namespace donner::css {

// --- ComponentValue ---

TEST(ComponentValueTest, TokenValue) {
  Token token(Token::Ident(RcString("hello")), 0);
  ComponentValue value(token);
  EXPECT_TRUE(std::holds_alternative<Token>(value.value));
}

TEST(ComponentValueTest, FunctionValue) {
  Function func("rgb", FileOffset::Offset(0));
  ComponentValue value(std::move(func));
  EXPECT_TRUE(std::holds_alternative<Function>(value.value));
}

TEST(ComponentValueTest, SimpleBlockValue) {
  SimpleBlock block(Token::indexOf<Token::Parenthesis>(), FileOffset::Offset(0));
  ComponentValue value(std::move(block));
  EXPECT_TRUE(std::holds_alternative<SimpleBlock>(value.value));
}

// --- Function ---

TEST(CssFunctionTest, EqualityByName) {
  Function a("rgb", FileOffset::Offset(0));
  Function b("rgb", FileOffset::Offset(0));
  EXPECT_EQ(a, b);
}

TEST(CssFunctionTest, InequalityByName) {
  Function a("rgb", FileOffset::Offset(0));
  Function b("hsl", FileOffset::Offset(0));
  EXPECT_NE(a, b);
}

TEST(CssFunctionTest, OstreamOutput) {
  Function func("rgb", FileOffset::Offset(0));
  std::ostringstream os;
  os << func;
  EXPECT_THAT(os.str(), testing::HasSubstr("rgb"));
}

// --- SimpleBlock ---

TEST(SimpleBlockTest, OstreamOutputParen) {
  SimpleBlock block(Token::indexOf<Token::Parenthesis>(), FileOffset::Offset(0));
  std::ostringstream os;
  os << block;
  EXPECT_THAT(os.str(), testing::HasSubstr("("));
}

TEST(SimpleBlockTest, OstreamOutputBracket) {
  SimpleBlock block(Token::indexOf<Token::SquareBracket>(), FileOffset::Offset(0));
  std::ostringstream os;
  os << block;
  EXPECT_THAT(os.str(), testing::HasSubstr("["));
}

TEST(SimpleBlockTest, OstreamOutputBrace) {
  SimpleBlock block(Token::indexOf<Token::CurlyBracket>(), FileOffset::Offset(0));
  std::ostringstream os;
  os << block;
  EXPECT_THAT(os.str(), testing::HasSubstr("{"));
}

// --- ComplexSelector specificity ---

TEST(ComplexSelectorTest, ParseAndComputeSpecificity) {
  auto result = parser::SelectorParser::Parse("#id .class element");
  ASSERT_TRUE(result.hasResult());

  const auto& selector = result.result();
  auto specificity = selector.maxSpecificity();
  // #id = (1,0,0), .class = (0,1,0), element = (0,0,1) => (1,1,1)
  EXPECT_EQ(specificity.a, 1);
  EXPECT_EQ(specificity.b, 1);
  EXPECT_EQ(specificity.c, 1);
}

TEST(ComplexSelectorTest, ClassOnlySpecificity) {
  auto result = parser::SelectorParser::Parse(".foo.bar");
  ASSERT_TRUE(result.hasResult());

  auto specificity = result.result().maxSpecificity();
  EXPECT_EQ(specificity.a, 0);
  EXPECT_EQ(specificity.b, 2);
  EXPECT_EQ(specificity.c, 0);
}

TEST(ComplexSelectorTest, TypeOnlySpecificity) {
  auto result = parser::SelectorParser::Parse("div");
  ASSERT_TRUE(result.hasResult());

  auto specificity = result.result().maxSpecificity();
  EXPECT_EQ(specificity.a, 0);
  EXPECT_EQ(specificity.b, 0);
  EXPECT_EQ(specificity.c, 1);
}

TEST(ComplexSelectorTest, UniversalSelectorSpecificity) {
  auto result = parser::SelectorParser::Parse("*");
  ASSERT_TRUE(result.hasResult());

  auto specificity = result.result().maxSpecificity();
  EXPECT_EQ(specificity.a, 0);
  EXPECT_EQ(specificity.b, 0);
  EXPECT_EQ(specificity.c, 0);
}

// --- PseudoClassSelector ---

TEST(PseudoClassSelectorTest, BasicPseudoClassSpecificity) {
  auto result = parser::SelectorParser::Parse(":hover");
  ASSERT_TRUE(result.hasResult());

  auto specificity = result.result().maxSpecificity();
  EXPECT_EQ(specificity.a, 0);
  EXPECT_EQ(specificity.b, 1);
  EXPECT_EQ(specificity.c, 0);
}

TEST(PseudoClassSelectorTest, WhereSpecificity) {
  auto result = parser::SelectorParser::Parse(":where(.foo)");
  ASSERT_TRUE(result.hasResult());

  auto specificity = result.result().maxSpecificity();
  EXPECT_EQ(specificity.a, 0);
  EXPECT_EQ(specificity.b, 0);
  EXPECT_EQ(specificity.c, 0);
}

TEST(PseudoClassSelectorTest, IsSpecificity) {
  auto result = parser::SelectorParser::Parse(":is(.foo, #bar)");
  ASSERT_TRUE(result.hasResult());

  auto specificity = result.result().maxSpecificity();
  EXPECT_EQ(specificity.a, 1);
  EXPECT_EQ(specificity.b, 0);
  EXPECT_EQ(specificity.c, 0);
}

TEST(PseudoClassSelectorTest, NotSpecificity) {
  auto result = parser::SelectorParser::Parse(":not(.foo)");
  ASSERT_TRUE(result.hasResult());

  auto specificity = result.result().maxSpecificity();
  EXPECT_EQ(specificity.a, 0);
  EXPECT_EQ(specificity.b, 1);
  EXPECT_EQ(specificity.c, 0);
}

// --- ComplexSelector ostream ---

TEST(ComplexSelectorTest, OstreamOutput) {
  auto result = parser::SelectorParser::Parse(".foo > .bar");
  ASSERT_TRUE(result.hasResult());

  std::ostringstream os;
  os << result.result();
  EXPECT_FALSE(os.str().empty());
}

}  // namespace donner::css
