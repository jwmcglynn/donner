#pragma once

#include <string_view>
#include <vector>

#include "src/css/declaration.h"

namespace donner {
namespace css {

class DeclarationListParser {
public:
  /**
   * Parse a HTML/SVG style attribute, corresponding to a CSS <declaration-list>.
   *
   * @param str Input string.
   * @return Parsed declaration list.
   */
  static std::vector<DeclarationOrAtRule> Parse(std::string_view str);

  /**
   * Parse a HTML/SVG style attribute, corresponding to a CSS <declaration-list>, but only returns
   * the list of declarations, skipping any at-rules when parsing.
   *
   * @param str Input string.
   * @return Parsed declaration list.
   */
  static std::vector<Declaration> ParseOnlyDeclarations(std::string_view str);

  /**
   * Parse a list of component values, from a Rule definition, corresponding to a CSS
   * <declaration-list>.
   *
   * @param components List of component values.
   * @return Parsed declaration list.
   */
  static std::vector<Declaration> ParseRuleDeclarations(std::span<ComponentValue> components);
};

}  // namespace css
}  // namespace donner
