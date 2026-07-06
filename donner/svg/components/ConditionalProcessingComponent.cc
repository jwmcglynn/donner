#include "donner/svg/components/ConditionalProcessingComponent.h"

#include "donner/base/StringUtils.h"

namespace donner::svg::components {

namespace {

using ConditionalProcessingField = std::optional<RcString> ConditionalProcessingComponent::*;

ConditionalProcessingField FieldForConditionalProcessingAttribute(
    const xml::XMLQualifiedNameRef& name) {
  if (name == xml::XMLQualifiedNameRef("requiredFeatures")) {
    return &ConditionalProcessingComponent::requiredFeatures;
  }
  if (name == xml::XMLQualifiedNameRef("requiredExtensions")) {
    return &ConditionalProcessingComponent::requiredExtensions;
  }
  if (name == xml::XMLQualifiedNameRef("systemLanguage")) {
    return &ConditionalProcessingComponent::systemLanguage;
  }

  return nullptr;
}

}  // namespace

bool IsConditionalProcessingAttribute(const xml::XMLQualifiedNameRef& name) {
  return FieldForConditionalProcessingAttribute(name) != nullptr;
}

bool SetConditionalProcessingAttribute(ConditionalProcessingComponent& conditional,
                                       const xml::XMLQualifiedNameRef& name,
                                       std::string_view value) {
  const ConditionalProcessingField field = FieldForConditionalProcessingAttribute(name);
  if (field == nullptr) {
    return false;
  }

  conditional.*field = RcString(value);
  return true;
}

bool RemoveConditionalProcessingAttribute(ConditionalProcessingComponent& conditional,
                                          const xml::XMLQualifiedNameRef& name) {
  const ConditionalProcessingField field = FieldForConditionalProcessingAttribute(name);
  if (field == nullptr) {
    return false;
  }

  conditional.*field = std::nullopt;
  return true;
}

bool HasConditionalProcessingAttributes(const ConditionalProcessingComponent& conditional) {
  return conditional.requiredFeatures.has_value() || conditional.requiredExtensions.has_value() ||
         conditional.systemLanguage.has_value();
}

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

bool EvaluateConditionalProcessing(const ConditionalProcessingComponent& conditional,
                                   std::span<const RcString> userLanguages) {
  // `requiredFeatures` is deprecated in SVG2 and always evaluates to true (matching resvg, which
  // ignores it) - intentionally not checked here.

  if (conditional.requiredExtensions.has_value()) {
    // Donner supports no extensions: any non-empty list evaluates to false. An empty string
    // evaluates to true per SVG 1.1.
    if (!StringUtils::TrimWhitespace(*conditional.requiredExtensions).empty()) {
      return false;
    }
  }

  if (conditional.systemLanguage.has_value()) {
    // The attribute passes if any user-preferred language matches. An empty language list
    // therefore evaluates any present `systemLanguage` to false.
    bool matched = false;
    for (const RcString& userLanguage : userLanguages) {
      if (SystemLanguageMatches(*conditional.systemLanguage, userLanguage)) {
        matched = true;
        break;
      }
    }
    if (!matched) {
      return false;
    }
  }

  return true;
}

bool EvaluateConditionalProcessing(const ConditionalProcessingComponent& conditional) {
  // Default user language list of `{"en"}`, matching resvg's default (the resvg-test-suite goldens
  // are rendered with it).
  static const std::vector<RcString> kDefaultUserLanguages = {RcString("en")};
  return EvaluateConditionalProcessing(conditional, kDefaultUserLanguages);
}

}  // namespace donner::svg::components
