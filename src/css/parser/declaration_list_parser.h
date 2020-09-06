#pragma once

#include <string_view>

#include "src/base/box.h"
#include "src/base/parser/parse_result.h"
#include "src/css/declaration.h"

namespace donner {
namespace css {

class DeclarationListParser {
public:
  /**
   * Parse a HTML/SVG style attribute, corresponding to a CSS <declaration-list>.
   *
   * @param str Input string.
   * @return Parsed declaration list, or an error.
   */
  static ParseResult<std::vector<Declaration>> Parse(std::string_view str);
};

}  // namespace css
}  // namespace donner
