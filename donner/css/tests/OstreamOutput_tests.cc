/**
 * @file Tests for ostream output operators across CSS types.
 *
 * Covers operator<< for types that don't have dedicated test files exercising their
 * string output: Combinator, AttrMatcher, Specificity::SpecialType, AnbValue, and
 * Stylesheet/SelectorRule.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/css/Specificity.h"
#include "donner/css/Stylesheet.h"
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

/// @test Ostream output \c operator<< for \ref ClassSelector.
TEST(ClassSelectorTest, OstreamOutput) {
  ClassSelector selector("highlight");
  EXPECT_THAT(selector, ToStringIs("ClassSelector(highlight)"));
}

/// @test Ostream output \c operator<< for \ref IdSelector.
TEST(IdSelectorTest, OstreamOutput) {
  IdSelector selector("main-content");
  EXPECT_THAT(selector, ToStringIs("IdSelector(main-content)"));
}

/// @test Ostream output \c operator<< for empty \ref Stylesheet.
TEST(StylesheetTest, OstreamOutputEmpty) {
  Stylesheet stylesheet;
  EXPECT_THAT(stylesheet, ToStringIs(""));
}

}  // namespace donner::css
