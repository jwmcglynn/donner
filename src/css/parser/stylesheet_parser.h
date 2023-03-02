#pragma once

#include <string_view>

#include "src/css/stylesheet.h"

namespace donner {
namespace css {

/**
 * Parse a CSS stylesheet into a list of selectors and declarations.
 */
class StylesheetParser {
public:
  /**
   * Parse a CSS stylesheet into a list of selectors and declarations.
   *
   * @param str Input stylesheet string.
   * @return Parsed stylesheet.
   */
  static Stylesheet Parse(std::string_view str);
};

}  // namespace css
}  // namespace donner
