#pragma once
/// @file

#include <string_view>

#include "src/css/stylesheet.h"

namespace donner::css {

/**
 * Public API for parsing CSS.
 */
class CSS {
public:
  /**
   * Parse a CSS stylesheet into a list of selectors and their associated declarations, which is
   * wrapped into a \ref Stylesheet object.
   *
   * @param str Input stylesheet string.
   * @return Parsed stylesheet.
   */
  static Stylesheet ParseStylesheet(std::string_view str);

  /**
   * Parse a `style=""` attribute into a list of \ref Declaration.
   *
   * For example:
   * ```
   * style="fill:red; stroke:blue"
   * ```
   *
   * Returns two declarations, one for `fill` and one for `stroke`.
   *
   * @param str Input style attribute string.
   * @return Parsed declarations.
   */
  static std::vector<Declaration> ParseStyleAttribute(std::string_view str);
};

}  // namespace donner::css
