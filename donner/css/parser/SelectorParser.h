#pragma once
/// @file

#include <span>
#include <string_view>

#include "donner/base/parser/ParseResult.h"
#include "donner/css/ComponentValue.h"
#include "donner/css/Selector.h"

namespace donner::css::parser {

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
   *
   * @param components The list of ComponentValues to parse.
   */
  static ParseResult<Selector> ParseComponents(std::span<const ComponentValue> components);

  /**
   * Parse CSS selector from a string.
   *
   * @param str The string to parse.
   */
  static ParseResult<Selector> Parse(std::string_view str);

  /**
   * Parse a "forgiving selector list", which is a list of selectors separated by commas, with
   * invalid selectors removed. This is different from the standard CSS behavior, where if a single
   * selector within a list of invalid, the entire selector list is ignored.
   *
   * For example, "div, p:invalid" will return a single selector, "div".
   *
   * @see https://www.w3.org/TR/selectors-4/#forgiving-selector for more details.
   *
   * @param components The list of ComponentValues to parse.
   */
  static Selector ParseForgivingSelectorList(std::span<const ComponentValue> components);

  /**
   * Parse a "forgiving relative selector list", which is a list of selectors separated by commas,
   * with invalid selectors removed. As relative selectors, these may start with a combinator, such
   * as `> div`.
   *
   * These can be matched with \ref Selector::matches with \ref
   * SelectorMatchOptions::relativeToElement set.
   *
   * @see https://www.w3.org/TR/selectors-4/#forgiving-selector for more details on
   * `<forgiving-relative-selector-list>`.
   */
  static Selector ParseForgivingRelativeSelectorList(std::span<const ComponentValue> components);
};

}  // namespace donner::css::parser
