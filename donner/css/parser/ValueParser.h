#pragma once
/// @file

#include <string_view>

#include "donner/css/ComponentValue.h"

namespace donner::css::parser {

/**
 * Parse a CSS value, per https://www.w3.org/TR/css-syntax-3/#parse-list-of-component-values. This
 * is used when parsing CSS-like attributes within XML/HTML, such as SVG presentation attributes.
 */
class ValueParser {
public:
  /**
   * Parse a CSS value, per https://www.w3.org/TR/css-syntax-3/#parse-list-of-component-values. This
   * is used when parsing CSS-like attributes within XML/HTML, such as SVG presentation attributes.
   *
   * For example, in SVG the following provide the same style:
   * ```xml
   * <circle r="10" style="fill:red" />
   * ```
   *
   * and:
   * ```xml
   * <circle r="10" fill="red" />
   * ```
   *
   * This function would parse the string "red" and return a list of component values representing
   * the value.
   *
   * @param str Input value string.
   * @return Parsed value as a list of component values.
   */
  static std::vector<ComponentValue> Parse(std::string_view str);
};

}  // namespace donner::css::parser
