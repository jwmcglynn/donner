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
   * Unconditional `@import` rules are retained in \ref Stylesheet::imports, in source order, with
   * their URLs exactly as authored; the parser never resolves or fetches them. Following the CSS
   * Cascade 5 grammar, an import is valid only before every other retained rule except `@layer`
   * statements (`@charset` is consumed first). Imports that appear later, have a malformed or empty
   * target, or carry `layer`, `supports()`, or media conditions other than `all` are dropped with a
   * bounded warning at the `@` token that never includes the target URL.
   *
   * CSS `@media` and `@supports` blocks are unsupported. Their complete blocks, including nested
   * styles and font faces, are ignored with a bounded warning at the outer `@` token.
   *
   * @param str Input stylesheet string.
   * @param warningSink Sink to collect warnings (e.g. invalid selectors).
   * @return Parsed stylesheet.
   */
  static Stylesheet Parse(std::string_view str, ParseWarningSink& warningSink);
};

}  // namespace donner::css::parser
