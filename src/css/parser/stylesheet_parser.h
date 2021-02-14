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

  /**
   * Parse a list of rules, per https://www.w3.org/TR/css-syntax-3/#parse-list-of-rules
   *
   * @param str Input list of rules string.
   * @return Parsed list of rules.
   */
  static std::vector<Rule> ParseListOfRules(std::string_view str);

  /**
   * Parse a rule, per https://www.w3.org/TR/css-syntax-3/#parse-rule
   *
   * @param str Input rule string.
   * @return Parsed rule, or std::nullopt if there was no rule.
   */
  static std::optional<Rule> ParseRule(std::string_view str);
};

}  // namespace css
}  // namespace donner
