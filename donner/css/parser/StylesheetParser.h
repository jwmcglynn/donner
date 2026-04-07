#pragma once
/// @file

#include <string_view>

#include "donner/base/ParseWarningSink.h"
#include "donner/css/Stylesheet.h"

namespace donner::css::parser {

/**
 * Parse a CSS stylesheet into a list of selectors and their associated declarations.
 */
class StylesheetParser {
public:
  /**
   * Parse a CSS stylesheet into a list of selectors and their associated declarations, as well as
   * `@font-face` rules.
   *
   * @param str Input stylesheet string.
   * @param warningSink Sink to collect warnings (e.g. invalid selectors).
   * @return Parsed stylesheet.
   */
  static Stylesheet Parse(std::string_view str, ParseWarningSink& warningSink);
};

}  // namespace donner::css::parser
