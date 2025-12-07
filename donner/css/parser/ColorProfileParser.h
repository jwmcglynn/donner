#pragma once
/// @file

#include <span>
#include <string_view>

#include "donner/css/ColorProfile.h"
#include "donner/css/Rule.h"

namespace donner::css::parser {

/**
 * Parse `@color-profile` rules from a stylesheet and produce a registry of profile aliases mapped
 * to the SVG2 color spaces that Donner supports.
 */
class ColorProfileParser {
public:
  /// Parse a list of rules that may contain `@color-profile` definitions.
  static ColorProfileRegistry Parse(std::span<const Rule> rules);

  /// Parse `@color-profile` definitions directly from a stylesheet string.
  static ColorProfileRegistry ParseStylesheet(std::string_view stylesheet);
};

}  // namespace donner::css::parser
