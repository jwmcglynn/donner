#pragma once
/// @file

#include <cstddef>
#include <string_view>
#include <vector>

#include "donner/css/Declaration.h"

namespace donner::css::parser {

/**
 * Parse a CSS declaration list, which can be either from an HTML/SVG style attribute, or from the
 * list of component values from within a rule.
 *
 * For example, the following is a valid declaration list:
 * ```
 * color: red; background-color: blue; font-size: 12px;
 * ```
 */
class DeclarationListParser {
public:
  /// Maximum declarations or at-rules retained from one declaration list.
  static constexpr std::size_t kMaximumDeclarations = 4096;
  /// Maximum component values parsed across one declaration list.
  static constexpr std::size_t kMaximumComponentValues = 64 * 1024;

  struct SecurityStats {
    std::size_t declarations = 0;
    std::size_t componentValues = 0;
    bool rejected = false;
  };

  /**
   * Parse a HTML/SVG style attribute, corresponding to a CSS <declaration-list>.
   *
   * @param str Input string.
   * @return Parsed declaration list.
   */
  static std::vector<DeclarationOrAtRule> Parse(std::string_view str,
                                                SecurityStats* securityStats = nullptr);

  /**
   * Parse a HTML/SVG style attribute, corresponding to a CSS <declaration-list>, but only returns
   * the list of declarations, skipping any at-rules when parsing.
   *
   * @param str Input string.
   * @return Parsed declaration list.
   */
  static std::vector<Declaration> ParseOnlyDeclarations(std::string_view str,
                                                        SecurityStats* securityStats = nullptr);

  /**
   * Parse a list of component values, from a Rule definition, corresponding to a CSS
   * <declaration-list>.
   *
   * @param components List of component values.
   * @return Parsed declaration list.
   */
  static std::vector<Declaration> ParseRuleDeclarations(std::span<ComponentValue> components,
                                                        SecurityStats* securityStats = nullptr);
};

}  // namespace donner::css::parser
