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
   * CSS `@import` is unsupported. Imports are ignored with a bounded warning at the `@` token;
   * import targets are not included in the warning and no resources are requested. A following
   * local rule is parsed after a semicolon-terminated import.
   *
   * CSS `@media` and `@supports` are also unsupported. Their complete blocks, including nested
   * styles and font faces, are ignored with a bounded warning at the outer `@` token.
   *
   * @param str Input stylesheet string.
   * @param warningSink Sink to collect warnings (e.g. invalid selectors).
   * @return Parsed stylesheet.
   */
  static Stylesheet Parse(std::string_view str, ParseWarningSink& warningSink);
};

}  // namespace donner::css::parser
