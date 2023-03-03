#pragma once

#include <variant>
#include <vector>

#include "src/css/declaration.h"

namespace donner {
namespace css {

/**
 * A QualifiedRule has a list of component values and a block, this is the intermediate
 * representation of a stylesheet rule.
 *
 * For example, for a stylesheet rule:
 * ```css
 * a > b { color: red }
 * ```
 *
 * - `a > b` is part of the prelude, as a tokenized list of \ref ComponentValue.
 * - The block contains `color: red`, tokenized, and an associatedToken of
 * `Token::indexOf<Token::CurlyBracket>`, or `<{-token}>`.
 */
struct QualifiedRule {
  /**
   * A list of component values before the block definition. For `a > b { color: red }`, `a > b` is
   * the prelude, and it would be parsed into five token \ref ComponentValue, `a, ' ', >, b, ' '`.
   */
  std::vector<ComponentValue> prelude;

  /**
   * Block with an opening associated token, such as '{', '[', or '(', and a list of tokens within
   * the block.
   *
   * For stylesheet rules, this is the '{ color: red }' part of the rule.
   */
  SimpleBlock block;

  /**
   * Constructor, takes ownership of the prelude and block.
   */
  QualifiedRule(std::vector<ComponentValue>&& prelude, SimpleBlock&& block)
      : prelude(prelude), block(block) {}

  /// Equality operator.
  bool operator==(const QualifiedRule& other) const = default;

  /**
   * Ostream output operator for the QualifiedRule, in a human-readable but verbose format, to be
   * used for debugging and testing.
   *
   * For example:
   * ```
   * QualifiedRule {
   *   a > b { color: red }
   * }
   * ```
   *
   * Would be:
   * ```cpp file=rule_parser_tests.cc
   * QualifiedRule {
   * Token { Ident(a) offset: 0 }
   * Token { Whitespace(' ', len=1) offset: 1 }
   * Token { Delim(>) offset: 2 }
   * Token { Whitespace(' ', len=1) offset: 3 }
   * Token { Ident(b) offset: 4 }
   * Token { Whitespace(' ', len=1) offset: 5 }
   * { SimpleBlock {
   * token='{'
   * Token { Whitespace(' ', len=1) offset: 7 }
   * Token { Ident(color) offset: 8 }
   * Token { Colon offset: 13 }
   * Token { Whitespace(' ', len=1) offset: 14 }
   * Token { Ident(red) offset: 15 }
   * Token { Whitespace(' ', len=1) offset: 18 }
   * } }
   * }
   * ```
   */
  friend std::ostream& operator<<(std::ostream& os, const QualifiedRule& qualifiedRule) {
    os << "QualifiedRule {\n";
    for (const auto& value : qualifiedRule.prelude) {
      os << "  " << value << "\n";
    }
    os << "  { " << qualifiedRule.block << " }\n";
    return os << "}";
  }
};

/**
 * Holds a CSS rule which can either be a standard QualifiedRule, an AtRule, or an InvalidRule if
 * there was a parse error.
 *
 * Examples:
 * - QualifiedRule: `a > b { color: red }`
 * - AtRule: `@media (min-width: 600px) { a > b { color: red } }`
 * - InvalidRule, in this case since `@charset` needs to end with a semicolon: `@charset \"123\"`
 */
struct Rule {
  /// Variant for the different rule types.
  using Type = std::variant<AtRule, QualifiedRule, InvalidRule>;

  /// Rule value.
  Type value;

  /**
   * Construct a new rule object from any type within the \ref Type variant.
   *
   * @param value Rule value.
   */
  /* implicit */ Rule(Type&& value) : value(value) {}

  /// Equality operator.
  bool operator==(const Rule& other) const = default;

  /// Ostream output operator.
  friend std::ostream& operator<<(std::ostream& os, const Rule& rule) {
    std::visit([&os](auto&& v) { os << v; }, rule.value);
    return os;
  }
};

}  // namespace css
}  // namespace donner
