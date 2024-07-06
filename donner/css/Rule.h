#pragma once
/// @file

#include <optional>
#include <variant>
#include <vector>

#include "donner/css/ComponentValue.h"

namespace donner::css {

/**
 * Rules starting with an `@` are called At-Rules, and are used to define CSS features such as
 * `@media`, `@font-face`, `@keyframes`, etc.
 *
 * For example, the following is a valid at-rule:
 * ```css
 * @media (min-width: 600px) {
 *  a > b { color: red }
 * }
 * ```
 *
 * Note that `@charset` is a special rule, which does not show up as an AtRule, but it is used to
 * inform the parsing behavior. For example, if the first rule is `@charset "utf-8";`, then this
 * will not show up in the list of rules, but the parser will be in UTF-8 mode.
 */
struct AtRule {
  /// Name of the at-rule, such as `media`, `font-face`, `keyframes`, etc.
  RcString name;

  /// List of component values before the block definition.
  std::vector<ComponentValue> prelude;

  /// Block for the at-rule's definition, if any.
  std::optional<SimpleBlock> block;

  /**
   * Construct the AtRule with the given name.
   *
   * @param name Name of the at-rule.
   */
  explicit AtRule(RcString name);

  /// Equality operator.
  bool operator==(const AtRule& other) const;

  /**
   * Output a human-readable parsed representation of AtRule to the given stream.
   *
   * @param os Output stream.
   * @param rule AtRule to output.
   */
  friend std::ostream& operator<<(std::ostream& os, const AtRule& rule);
};

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
      : prelude(std::move(prelude)), block(std::move(block)) {}

  /// Destructor
  ~QualifiedRule() = default;

  // Copy and move constructors.
  QualifiedRule(const QualifiedRule& other) = default;
  QualifiedRule& operator=(const QualifiedRule& other) = default;
  QualifiedRule(QualifiedRule&& other) = default;
  QualifiedRule& operator=(QualifiedRule&& other) = default;

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
 * InvalidRule is used to represent a rule which could not be parsed, such as an invalid at-rule.
 *
 * For example, the following is an invalid at-rule:
 * ```css
 * @charset "123"
 * ```
 *
 * The `@charset` is a valid at-rule, but it is missing a semicolon at the end, so it is invalid.
 *
 * The InvalidRule may have a more specific type, such as `ExtraInput`, depending on how the error
 * was triggered.
 */
struct InvalidRule {
  /**
   * Type of the invalid rule.
   */
  enum class Type {
    Default,    ///< Default type, no specific information.
    ExtraInput  ///< The rule had extra input after the end of the rule.
  };

  /// Type of the invalid rule.
  Type type;

  /**
   * Construct an InvalidRule with the given type.
   *
   * @param type Type of the invalid rule.
   */
  explicit InvalidRule(Type type = Type::Default) : type(type) {}

  /// Equality operator.
  bool operator==(const InvalidRule& other) const { return type == other.type; }

  /**
   * Output a human-readable parsed representation of InvalidRule to the given stream.
   *
   * For example, `InvalidRule` or `InvalidRule(ExtraInput)`.
   *
   * @param os Output stream.
   * @param invalidRule InvalidRule to output.
   */
  friend std::ostream& operator<<(std::ostream& os, const InvalidRule& invalidRule) {
    os << "InvalidRule";
    if (invalidRule.type == InvalidRule::Type::ExtraInput) {
      os << "(ExtraInput)";
    }
    return os;
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
  /* implicit */ Rule(Type&& value) : value(std::move(value)) {}

  /// Destructor
  ~Rule() = default;

  // Copy and move constructors and operators.
  Rule(const Rule& other) = default;
  Rule(Rule&& other) noexcept = default;
  Rule& operator=(const Rule& other) = default;
  Rule& operator=(Rule&& other) noexcept = default;

  /// Equality operator.
  bool operator==(const Rule& other) const = default;

  /// Ostream output operator.
  friend std::ostream& operator<<(std::ostream& os, const Rule& rule) {
    std::visit([&os](auto&& v) { os << v; }, rule.value);
    return os;
  }
};

}  // namespace donner::css
