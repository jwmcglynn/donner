#pragma once

#include <string_view>

#include "src/base/box.h"
#include "src/svg/parser/parse_result.h"

namespace donner {

class ViewboxParser {
public:
  /**
   * Parse an SVG viewBox attribute.
   *
   * See https://www.w3.org/TR/SVG/coords.html#ViewBoxAttribute
   *
   * It parses a string containing the following values:
   *  <min-x>,? <min-y>,? <width>,? <height>
   *
   * Each parameter is a <number> type, as parsed by NumberParser. <width> and <height> must be
   * positive, but the caller must ensure that they are non-zero; a value of zero should disable
   * rendering of the element.
   *
   * @return Parsed box.
   */
  static ParseResult<Boxd> Parse(std::string_view d);
};

}  // namespace donner
