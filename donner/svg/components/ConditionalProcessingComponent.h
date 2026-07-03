#pragma once
/// @file

#include <optional>

#include "donner/base/RcString.h"

namespace donner::svg::components {

/**
 * Stores the SVG conditional-processing attributes (`requiredFeatures`, `requiredExtensions`,
 * `systemLanguage`) for an element. Attached only when at least one of the attributes is present.
 *
 * These attributes control whether an element renders: on a direct child of \ref xml_switch they
 * select the first matching child, and on any other rendered element they disable rendering of
 * the element (and its subtree) when they evaluate to false.
 *
 * See https://www.w3.org/TR/SVG2/struct.html#ConditionalProcessing
 */
struct ConditionalProcessingComponent {
  /**
   * Raw `requiredFeatures` attribute value. Deprecated in SVG2; always evaluates to true,
   * matching resvg's behavior. Stored so the DOM state reflects the document.
   */
  std::optional<RcString> requiredFeatures;

  /**
   * Raw `requiredExtensions` attribute value, a whitespace-separated list of extension IRIs.
   * Donner supports no extensions, so any non-empty value evaluates to false.
   */
  std::optional<RcString> requiredExtensions;

  /**
   * Raw `systemLanguage` attribute value, a comma-separated list of BCP 47 language tags.
   * Evaluates to true if any listed tag matches a user-preferred language.
   */
  std::optional<RcString> systemLanguage;
};

/**
 * Evaluates an element's conditional-processing attributes.
 *
 * Matches resvg's evaluation rules so rendering agrees with the resvg-test-suite goldens:
 * - `requiredFeatures` always evaluates to true (deprecated in SVG2, ignored).
 * - `requiredExtensions` evaluates to true only when empty (no extensions are supported).
 * - `systemLanguage` evaluates to true if any comma-separated tag equals a user language or is a
 *   sub-tag of it (e.g. both "en" and "en-GB" match the default user language "en").
 *
 * @param conditional Conditional-processing attribute values to evaluate.
 * @return true if all present attributes evaluate to true (the element may render).
 */
bool EvaluateConditionalProcessing(const ConditionalProcessingComponent& conditional);

/**
 * Evaluates a single `systemLanguage` attribute value against the user-preferred language.
 *
 * @param systemLanguage Comma-separated list of BCP 47 language tags (e.g. "ru, en").
 * @param userLanguage User-preferred language tag to match against (e.g. "en"). A document tag
 *   matches if it equals \p userLanguage or starts with `userLanguage + "-"`, compared
 *   case-insensitively.
 * @return true if any tag in the list matches the user language.
 */
bool SystemLanguageMatches(std::string_view systemLanguage, std::string_view userLanguage);

}  // namespace donner::svg::components
