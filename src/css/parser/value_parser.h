#pragma once

#include <string_view>

#include "src/css/declaration.h"

namespace donner {
namespace css {

class ValueParser {
public:
  /**
   * Parse a CSS value, per https://www.w3.org/TR/css-syntax-3/#parse-list-of-component-values. This
   * is used when parsing CSS-like attributes within XML/HTML, such as SVG presentation attributes.
   *
   * For example, in SVG the following provide the same style:
   * @code{.xml}
   * <circle r="10" style="fill:red" />
   * @endcode
   *
   * and:
   * @code{.xml}
   * <circle r="10" fill="red" />
   * @endcode
   *
   * This function would parse the string "red" and return a list of component values representing
   * the value.
   *
   * @param str Input value string.
   * @return Parsed value as a list of component values.
   */
  static std::vector<ComponentValue> Parse(std::string_view str);
};

}  // namespace css
}  // namespace donner