/**
 * Tests for PropertyParsing utility functions.
 */

#include "donner/svg/properties/PropertyParsing.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/css/ComponentValue.h"
#include "donner/css/Declaration.h"
#include "donner/css/Token.h"
#include "donner/svg/parser/SVGParser.h"

using testing::DoubleNear;
using testing::Eq;

namespace donner::svg::parser {

namespace {

css::ComponentValue makeToken(css::Token::TokenValue&& value) {
  return css::ComponentValue(css::Token(std::move(value), 0));
}

}  // namespace

// --- ParseAlphaValue ---

TEST(PropertyParsingTest, ParseAlphaValueNumber) {
  css::Declaration decl("opacity");
  decl.values.push_back(makeToken(css::Token::Number(0.5, "0.5", css::NumberType::Number)));

  auto params = PropertyParseFnParams::Create(decl, css::Specificity{},
                                               PropertyParseBehavior::AllowUserUnits);
  auto result = ParseAlphaValue(params.components());
  ASSERT_TRUE(result.hasResult());
  EXPECT_THAT(result.result(), DoubleNear(0.5, 0.001));
}

TEST(PropertyParsingTest, ParseAlphaValuePercentage) {
  css::Declaration decl("opacity");
  decl.values.push_back(makeToken(css::Token::Percentage(50.0, "50", css::NumberType::Number)));

  auto params = PropertyParseFnParams::Create(decl, css::Specificity{},
                                               PropertyParseBehavior::AllowUserUnits);
  auto result = ParseAlphaValue(params.components());
  ASSERT_TRUE(result.hasResult());
  EXPECT_THAT(result.result(), DoubleNear(0.5, 0.001));
}

TEST(PropertyParsingTest, ParseAlphaValueClampedHigh) {
  css::Declaration decl("opacity");
  decl.values.push_back(makeToken(css::Token::Number(2.0, "2.0", css::NumberType::Number)));

  auto params = PropertyParseFnParams::Create(decl, css::Specificity{},
                                               PropertyParseBehavior::AllowUserUnits);
  auto result = ParseAlphaValue(params.components());
  ASSERT_TRUE(result.hasResult());
  EXPECT_THAT(result.result(), DoubleNear(1.0, 0.001));
}

TEST(PropertyParsingTest, ParseAlphaValueClampedLow) {
  css::Declaration decl("opacity");
  decl.values.push_back(makeToken(css::Token::Number(-1.0, "-1.0", css::NumberType::Number)));

  auto params = PropertyParseFnParams::Create(decl, css::Specificity{},
                                               PropertyParseBehavior::AllowUserUnits);
  auto result = ParseAlphaValue(params.components());
  ASSERT_TRUE(result.hasResult());
  EXPECT_THAT(result.result(), DoubleNear(0.0, 0.001));
}

TEST(PropertyParsingTest, ParseAlphaValueInvalid) {
  css::Declaration decl("opacity");
  decl.values.push_back(makeToken(css::Token::Ident(RcString("auto"))));

  auto params = PropertyParseFnParams::Create(decl, css::Specificity{},
                                               PropertyParseBehavior::AllowUserUnits);
  auto result = ParseAlphaValue(params.components());
  EXPECT_TRUE(result.hasError());
}

// --- CSS-wide keyword detection ---

TEST(PropertyParsingTest, CssWideKeywordInitial) {
  css::Declaration decl("fill");
  decl.values.push_back(makeToken(css::Token::Ident(RcString("initial"))));

  auto params = PropertyParseFnParams::Create(decl, css::Specificity{});
  EXPECT_EQ(params.explicitState, PropertyState::ExplicitInitial);
}

TEST(PropertyParsingTest, CssWideKeywordInherit) {
  css::Declaration decl("fill");
  decl.values.push_back(makeToken(css::Token::Ident(RcString("inherit"))));

  auto params = PropertyParseFnParams::Create(decl, css::Specificity{});
  EXPECT_EQ(params.explicitState, PropertyState::Inherit);
}

TEST(PropertyParsingTest, CssWideKeywordUnset) {
  css::Declaration decl("fill");
  decl.values.push_back(makeToken(css::Token::Ident(RcString("unset"))));

  auto params = PropertyParseFnParams::Create(decl, css::Specificity{});
  EXPECT_EQ(params.explicitState, PropertyState::ExplicitUnset);
}

TEST(PropertyParsingTest, NonCssWideKeyword) {
  css::Declaration decl("fill");
  decl.values.push_back(makeToken(css::Token::Ident(RcString("red"))));

  auto params = PropertyParseFnParams::Create(decl, css::Specificity{});
  EXPECT_EQ(params.explicitState, PropertyState::NotSet);
}

// --- CreateForAttribute ---

TEST(PropertyParsingTest, CreateForAttributeInitial) {
  auto params = PropertyParseFnParams::CreateForAttribute("initial");
  EXPECT_EQ(params.explicitState, PropertyState::ExplicitInitial);
  EXPECT_TRUE(params.allowUserUnits());
}

TEST(PropertyParsingTest, CreateForAttributeInherit) {
  auto params = PropertyParseFnParams::CreateForAttribute("inherit");
  EXPECT_EQ(params.explicitState, PropertyState::Inherit);
}

TEST(PropertyParsingTest, CreateForAttributeNormalValue) {
  auto params = PropertyParseFnParams::CreateForAttribute("10px");
  EXPECT_EQ(params.explicitState, PropertyState::NotSet);
}

// --- TryGetSingleIdent ---

TEST(PropertyParsingTest, TryGetSingleIdentSuccess) {
  css::Declaration decl("display");
  decl.values.push_back(makeToken(css::Token::Ident(RcString("none"))));

  auto params = PropertyParseFnParams::Create(decl, css::Specificity{});
  auto ident = TryGetSingleIdent(params.components());
  ASSERT_TRUE(ident.has_value());
  EXPECT_EQ(ident.value(), "none");
}

TEST(PropertyParsingTest, TryGetSingleIdentFailsOnNumber) {
  css::Declaration decl("opacity");
  decl.values.push_back(makeToken(css::Token::Number(0.5, "0.5", css::NumberType::Number)));

  auto params = PropertyParseFnParams::Create(decl, css::Specificity{});
  auto ident = TryGetSingleIdent(params.components());
  EXPECT_FALSE(ident.has_value());
}

TEST(PropertyParsingTest, TryGetSingleIdentFailsOnMultiple) {
  css::Declaration decl("fill");
  decl.values.push_back(makeToken(css::Token::Ident(RcString("url"))));
  decl.values.push_back(makeToken(css::Token::Ident(RcString("none"))));

  auto params = PropertyParseFnParams::Create(decl, css::Specificity{});
  auto ident = TryGetSingleIdent(params.components());
  EXPECT_FALSE(ident.has_value());
}

// --- Integration: attributes parsed via SVG document ---

TEST(PropertyParsingIntegrationTest, PresentationAttributesParsedCorrectly) {
  ParseWarningSink warningSink;
  auto maybeResult = SVGParser::ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" width="50" height="50" fill="red" stroke="blue"
            opacity="0.5" stroke-width="2"/>
    </svg>
  )", warningSink);
  ASSERT_TRUE(maybeResult.hasResult());
  auto document = std::move(maybeResult.result());
  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
}

TEST(PropertyParsingIntegrationTest, StyleAttributeOverridesPresentation) {
  ParseWarningSink warningSink;
  auto maybeResult = SVGParser::ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" width="50" height="50" fill="red" style="fill: blue"/>
    </svg>
  )", warningSink);
  ASSERT_TRUE(maybeResult.hasResult());
  auto document = std::move(maybeResult.result());
  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
}

// --- ParseLengthPercentageOrAuto ---

TEST(PropertyParsingTest, ParseLengthPercentageOrAutoWithAuto) {
  css::Declaration decl("width");
  decl.values.push_back(makeToken(css::Token::Ident(RcString("auto"))));

  auto params = PropertyParseFnParams::Create(decl, css::Specificity{},
                                               PropertyParseBehavior::AllowUserUnits);
  auto result = ParseLengthPercentageOrAuto(params.components(), true);
  ASSERT_TRUE(result.hasResult());
  EXPECT_FALSE(result.result().has_value());
}

}  // namespace donner::svg::parser
