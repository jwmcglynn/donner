#include "donner/svg/components/ConditionalProcessingComponent.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using testing::Not;

namespace donner::svg::components {

namespace {

/**
 * Matches a `systemLanguage` attribute value that evaluates to true against the given
 * user-preferred language, per \ref SystemLanguageMatches.
 */
MATCHER_P(MatchesUserLanguage, userLanguage,
          std::string("systemLanguage value ") + (negation ? "does not match" : "matches") +
              " user language '" + std::string(userLanguage) + "'") {
  return SystemLanguageMatches(arg, userLanguage);
}

/**
 * Matches a \ref ConditionalProcessingComponent whose attributes all evaluate to true, per \ref
 * EvaluateConditionalProcessing.
 */
MATCHER(EvaluatesToTrue, std::string("conditional-processing attributes evaluate to ") +
                             (negation ? "false" : "true")) {
  return EvaluateConditionalProcessing(arg);
}

TEST(SystemLanguageMatchesTest, ExactMatch) {
  EXPECT_THAT("en", MatchesUserLanguage("en"));
  EXPECT_THAT("ru", Not(MatchesUserLanguage("en")));
}

TEST(SystemLanguageMatchesTest, SubtagOfUserLanguageMatches) {
  // A document tag more specific than the user language still matches ("en-GB" matches user
  // language "en"), following resvg's evaluation of the resvg-test-suite goldens.
  EXPECT_THAT("en-GB", MatchesUserLanguage("en"));
  EXPECT_THAT("en-US", MatchesUserLanguage("en"));
  EXPECT_THAT("ru-RU", Not(MatchesUserLanguage("en")));
}

TEST(SystemLanguageMatchesTest, PrefixWithoutSubtagSeparatorDoesNotMatch) {
  // "eng" shares the prefix "en" but is a different language tag.
  EXPECT_THAT("eng", Not(MatchesUserLanguage("en")));
}

TEST(SystemLanguageMatchesTest, CommaSeparatedListMatchesAnyTag) {
  EXPECT_THAT("ru, en", MatchesUserLanguage("en"));
  EXPECT_THAT("ru,en-GB", MatchesUserLanguage("en"));
  EXPECT_THAT("ru, fr", Not(MatchesUserLanguage("en")));
}

TEST(SystemLanguageMatchesTest, MatchIsCaseInsensitive) {
  EXPECT_THAT("EN", MatchesUserLanguage("en"));
  EXPECT_THAT("En-gb", MatchesUserLanguage("en"));
}

TEST(SystemLanguageMatchesTest, EmptyValueDoesNotMatch) {
  EXPECT_THAT("", Not(MatchesUserLanguage("en")));
  EXPECT_THAT("  ", Not(MatchesUserLanguage("en")));
  EXPECT_THAT(",,", Not(MatchesUserLanguage("en")));
}

TEST(EvaluateConditionalProcessingTest, NoAttributesEvaluatesToTrue) {
  EXPECT_THAT(ConditionalProcessingComponent{}, EvaluatesToTrue());
}

TEST(EvaluateConditionalProcessingTest, RequiredFeaturesIsIgnored) {
  // requiredFeatures is deprecated in SVG2 and always evaluates to true, matching resvg
  // (structure/switch/requiredFeatures.svg renders the child which declares a feature string).
  ConditionalProcessingComponent conditional;
  conditional.requiredFeatures = "http://www.w3.org/TR/SVG11/feature#Structure";
  EXPECT_THAT(conditional, EvaluatesToTrue());

  conditional.requiredFeatures = "http://example.org/bogus";
  EXPECT_THAT(conditional, EvaluatesToTrue());
}

TEST(EvaluateConditionalProcessingTest, NonEmptyRequiredExtensionsEvaluatesToFalse) {
  ConditionalProcessingComponent conditional;
  conditional.requiredExtensions = "http://example.org/bogus";
  EXPECT_THAT(conditional, Not(EvaluatesToTrue()));
}

TEST(EvaluateConditionalProcessingTest, EmptyRequiredExtensionsEvaluatesToTrue) {
  // Per SVG 1.1, an empty requiredExtensions list evaluates to true.
  ConditionalProcessingComponent conditional;
  conditional.requiredExtensions = "";
  EXPECT_THAT(conditional, EvaluatesToTrue());

  conditional.requiredExtensions = "  ";
  EXPECT_THAT(conditional, EvaluatesToTrue());
}

TEST(EvaluateConditionalProcessingTest, SystemLanguageEvaluatedAgainstUserLanguage) {
  ConditionalProcessingComponent conditional;
  conditional.systemLanguage = "en-GB";
  EXPECT_THAT(conditional, EvaluatesToTrue());

  conditional.systemLanguage = "ru-RU";
  EXPECT_THAT(conditional, Not(EvaluatesToTrue()));

  conditional.systemLanguage = "";
  EXPECT_THAT(conditional, Not(EvaluatesToTrue()));
}

TEST(EvaluateConditionalProcessingTest, AllPresentAttributesMustEvaluateToTrue) {
  ConditionalProcessingComponent conditional;
  conditional.systemLanguage = "en";
  conditional.requiredExtensions = "http://example.org/bogus";
  EXPECT_THAT(conditional, Not(EvaluatesToTrue()));

  conditional.requiredExtensions = "";
  EXPECT_THAT(conditional, EvaluatesToTrue());
}

}  // namespace

}  // namespace donner::svg::components
