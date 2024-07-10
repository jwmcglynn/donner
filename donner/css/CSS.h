#pragma once
/// @file

#include <string_view>

#include "donner/css/Stylesheet.h"

/**
 * Donner CSS library, a standalone composable CSS parser.
 *
 * This library is designed to be used in other projects, and provides building blocks to add CSS
 * parsing to any application. This is used by Donner itself to parse SVG stylesheets, but it can be
 * used for HTML-based CSS as well.
 *
 * This library includes support for parsing rules and declarations, as well as matching selectors
 * against a DOM tree.
 *
 * To get started, parse a Stylesheet using \ref CSS::ParseStylesheet, and then use the returned
 * \ref Stylesheet object to match against a DOM tree:
 * ```
 * auto stylesheet = CSS::ParseStylesheet("svg { fill: red; }");
 * for (const css::SelectorRule& rule : stylesheet.rules()) {
 *   if (css::SelectorMatchResult match = rule.selector.matches(domNode)) {
 *     applyDeclaration(rule.declarations, match.specificity);
 *   }
 * }
 * ```
 */
namespace donner::css {

/**
 * Public API for parsing CSS.
 */
class CSS {
public:
  /**
   * Parse a CSS stylesheet into a list of selectors and their associated declarations, which is
   * wrapped into a \ref Stylesheet object.
   *
   * @param str Input stylesheet string.
   * @return Parsed stylesheet.
   */
  static Stylesheet ParseStylesheet(std::string_view str);

  /**
   * Parse a `style=""` attribute into a list of \ref Declaration.
   *
   * For example:
   * ```
   * style="fill:red; stroke:blue"
   * ```
   *
   * Returns two declarations, one for `fill` and one for `stroke`.
   *
   * @param str Input style attribute string.
   * @return Parsed declarations.
   */
  static std::vector<Declaration> ParseStyleAttribute(std::string_view str);

  /**
   * Parse a CSS selector string into a \ref Selector object, which can be used to implement
   * querySelector and similar APIs.
   *
   * @param str Input selector string, e.g. "svg > rect".
   * @return Parsed selector, or std::nullopt if the selector is invalid.
   */
  static std::optional<Selector> ParseSelector(std::string_view str);
};

}  // namespace donner::css
