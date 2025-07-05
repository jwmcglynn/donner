#pragma once
/// @file

#include <compare>
#include <cstdint>
#include <ostream>

#include "donner/css/Declaration.h"
#include "donner/css/FontFace.h"
#include "donner/css/Selector.h"

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

  /**
   * Output a human-readable representation of the delector to a stream.
   *
   * @param os Output stream.
   * @param rule SelectorRule to output.
   */
  friend std::ostream& operator<<(std::ostream& os, const SelectorRule& rule) {
    os << rule.selector << " {\n";
    for (const auto& declaration : rule.declarations) {
      os << declaration << "\n";
    }
    return os << "}\n";
  }
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
  explicit Stylesheet(std::vector<SelectorRule>&& rules, std::vector<FontFace>&& fontFaces = {})
      : rules_(std::move(rules)), fontFaces_(std::move(fontFaces)) {}

  // Copyable and moveable.
  /// Copy constructor.
  Stylesheet(const Stylesheet&) = default;
  /// Copy assignment operator.
  Stylesheet& operator=(const Stylesheet&) = default;
  /// Move constructor.
  Stylesheet(Stylesheet&&) noexcept = default;
  /// Move assignment operator.
  Stylesheet& operator=(Stylesheet&&) noexcept = default;

  /// Destructor.
  ~Stylesheet() = default;

  /**
   * Get the list of rules in this stylesheet.
   */
  std::span<const SelectorRule> rules() const { return rules_; }

  /**
   * Get the list of @font-face rules in this stylesheet.
   */
  std::span<const FontFace> fontFaces() const { return fontFaces_; }

  /**
   * Output a human-readable representation of the stylesheet to a stream.
   *
   * @param os Output stream.
   * @param stylesheet Stylesheet to output.
   */
  friend std::ostream& operator<<(std::ostream& os, const Stylesheet& stylesheet) {
    for (const auto& rule : stylesheet.rules()) {
      os << rule << "\n";
    }
    return os;
  }

private:
  std::vector<SelectorRule> rules_;
  std::vector<FontFace> fontFaces_;
};

}  // namespace donner::css
