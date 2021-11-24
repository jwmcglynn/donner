#pragma once

#include <span>
#include <string_view>

#include "src/base/parser/parse_result.h"
#include "src/css/declaration.h"
#include "src/css/selector.h"

namespace donner {
namespace css {

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
