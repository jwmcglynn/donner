#pragma once
/// @file

#include <compare>
#include <cstdint>
#include <ostream>

#include "src/css/declaration.h"
#include "src/css/selector.h"

namespace donner::css {

/**
 * A rule in a stylesheet, which consists of a selector and a list of declarations.
 *
 * For example, the following is a valid rule:
 * ```
 * path.withColor {
 *   fill: red;
 *   stroke: blue;
 * }
 * ```
 *
 * The selector is `path.withColor`, and the declarations are `fill: red` and `stroke: blue`.
 */
struct SelectorRule {
  Selector selector;                      ///< Selector for this rule.
  std::vector<Declaration> declarations;  ///< Declarations for this rule.
};

/**
 * A CSS stylesheet, which is a list of rules. This is created by the parser, from the \ref
 * CSS::ParseStylesheet() API.
 */
class Stylesheet {
public:
  /// Default constructor.
  Stylesheet() = default;

  /**
   * Construct a stylesheet from a list of rules.
   *
   * @param rules List of rules, ownership is taken.
   */
  explicit Stylesheet(std::vector<SelectorRule>&& rules) : rules_(std::move(rules)) {}

  /// Default move constructor.
  Stylesheet(Stylesheet&&) = default;

  /// Default move assignment operator.
  Stylesheet& operator=(Stylesheet&&) = default;

  /**
   * Get the list of rules in this stylesheet.
   */
  std::span<const SelectorRule> rules() const { return rules_; }

private:
  std::vector<SelectorRule> rules_;
};

}  // namespace donner::css
