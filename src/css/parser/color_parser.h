#pragma once

#include <span>
#include <string_view>

#include "src/base/parser/parse_result.h"
#include "src/css/color.h"

namespace donner::css {

class ColorParser {
public:
  /**
   * Parse a CSS color, per https://www.w3.org/TR/2021/WD-css-color-4-20210601/
   *
   * @param components List of component values from the color declaration.
   * @return Parsed color.
   */
  static ParseResult<Color> Parse(std::span<const ComponentValue> components);

  /**
   * Parse a CSS color from a string, per https://www.w3.org/TR/2021/WD-css-color-4-20210601/
   *
   * @param str String that can be parsed into a list color declaration.
   * @return Parsed color.
   */
  static ParseResult<Color> ParseString(std::string_view str);
};

}  // namespace donner::css
