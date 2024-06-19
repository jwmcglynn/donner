#pragma once
/// @file

#include <string_view>

#include "donner/css/Stylesheet.h"

namespace donner::css::parser {

/**
 * Parse a CSS stylesheet into a list of selectors and their associated declarations.
 */
class StylesheetParser {
public:
  /**
   * Parse a CSS stylesheet into a list of selectors and their associated declarations.
   *
   * @param str Input stylesheet string.
   * @return Parsed stylesheet.
   */
  static Stylesheet Parse(std::string_view str);
};

}  // namespace donner::css::parser
