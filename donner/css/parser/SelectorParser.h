#pragma once
/// @file

#include <span>
#include <string_view>

#include "donner/base/parser/ParseResult.h"
#include "donner/css/ComponentValue.h"
#include "donner/css/Selector.h"

namespace donner::css::parser {

/**
 * Parse a CSS selector, or list of selectors, and returns a \ref Selector that can be matched
 * against in the stylesheet.
 *
 * Parses either from a string, or from the CSS intermediate representation, a list of
 * ComponentValues.
 *
 * For example, valid selectors may be "div", "div > p", "div > p:first-child", "div >
 * p:first-child:hover", etc. See https://www.w3.org/TR/selectors-4/#parse-selector for more
 * details.
 */
class SelectorParser {
public:
  /**
   * Parse CSS selector from a list of ComponentValues, see
   * https://www.w3.org/TR/selectors-4/#parse-selector.
   */
  static ParseResult<Selector> ParseComponents(std::span<const ComponentValue> components);

  /**
   * Parse CSS selector from a string.
   */
  static ParseResult<Selector> Parse(std::string_view str);
};

}  // namespace donner::css::parser
