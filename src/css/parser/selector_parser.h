#pragma once

#include <span>
#include <string_view>

#include "src/base/parser/parse_result.h"
#include "src/css/component_value.h"
#include "src/css/selector.h"

namespace donner {
namespace css {

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
  static ParseResult<Selector> Parse(std::span<const ComponentValue> components);

  /**
   * Parse CSS selector from a string.
   */
  static ParseResult<Selector> Parse(std::string_view str);
};

}  // namespace css
}  // namespace donner
