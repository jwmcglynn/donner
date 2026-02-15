#pragma once
/// @file

#include "donner/css/ColorProfile.h"
#include "donner/css/Rule.h"

namespace donner::css::parser {

/**
 * Utilities for registering CSS `@color-profile` definitions onto a shared registry.
 */
class ColorProfileParser {
public:
  /**
   * Parse a single at-rule and register its color profile definition if present.
   *
   * @param atRule At-rule to inspect.
   * @param registry Registry to populate when parsing succeeds.
   * @return \c true when an @color-profile rule was parsed and registered.
   */
  static bool ParseIntoRegistry(const AtRule& atRule, ColorProfileRegistry& registry);
};

}  // namespace donner::css::parser
