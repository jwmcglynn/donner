#pragma once

#include <string_view>

#include "src/css/stylesheet.h"

namespace donner {
namespace css {

class StylesheetParser {
public:
  /**
   * Parse a CSS stylesheet, per https://www.w3.org/TR/css-syntax-3/#parse-stylesheet
   *
   * @param str Input stylesheet string.
   * @return Parsed stylesheet.
   */
  static Stylesheet Parse(std::string_view str);
};

}  // namespace css
}  // namespace donner
