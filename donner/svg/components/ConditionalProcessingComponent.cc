#include "donner/svg/components/ConditionalProcessingComponent.h"

#include "donner/base/StringUtils.h"

namespace donner::svg::components {

namespace {

/**
 * The user-preferred language used to evaluate `systemLanguage`, matching resvg's default
 * language list of `["en"]`, which the resvg-test-suite goldens are rendered with.
 */
constexpr std::string_view kUserLanguage = "en";

}  // namespace

bool SystemLanguageMatches(std::string_view systemLanguage, std::string_view userLanguage) {
  for (const std::string_view rawTag : StringUtils::Split(systemLanguage, ',')) {
    const std::string_view tag = StringUtils::TrimWhitespace(rawTag);
    if (tag.empty()) {
      continue;
    }

    // A tag matches if it equals the user language, or if the user language is a prefix followed
    // by a subtag separator (e.g. tag "en-GB" matches user language "en"). Language tags are
    // case-insensitive per BCP 47.
    if (tag.size() == userLanguage.size()) {
      if (StringUtils::Equals<StringComparison::IgnoreCase>(tag, userLanguage)) {
        return true;
      }
    } else if (tag.size() > userLanguage.size() && tag[userLanguage.size()] == '-') {
      if (StringUtils::Equals<StringComparison::IgnoreCase>(tag.substr(0, userLanguage.size()),
                                                            userLanguage)) {
        return true;
      }
    }
  }

  return false;
}

bool EvaluateConditionalProcessing(const ConditionalProcessingComponent& conditional) {
  // `requiredFeatures` is deprecated in SVG2 and always evaluates to true (matching resvg, which
  // ignores it) — intentionally not checked here.

  if (conditional.requiredExtensions.has_value()) {
    // Donner supports no extensions: any non-empty list evaluates to false. An empty string
    // evaluates to true per SVG 1.1.
    if (!StringUtils::TrimWhitespace(*conditional.requiredExtensions).empty()) {
      return false;
    }
  }

  if (conditional.systemLanguage.has_value()) {
    if (!SystemLanguageMatches(*conditional.systemLanguage, kUserLanguage)) {
      return false;
    }
  }

  return true;
}

}  // namespace donner::svg::components
