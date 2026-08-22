#pragma once
/// @file

#include <cstddef>
#include <ostream>

#include "donner/base/Utils.h"
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
  SourceRange ruleSourceRange{FileOffset::Offset(0),
                              FileOffset::Offset(0)};  ///< Rule range in local CSS source.
  SourceRange selectorSourceRange{
      FileOffset::Offset(0), FileOffset::Offset(0)};  ///< Full selector range in local CSS source.
  std::vector<SourceRange>
      selectorEntrySourceRanges;  ///< Per-entry selector ranges in local CSS source.

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
  /// Resource-limit accounting produced while parsing an untrusted stylesheet.
  struct SecurityStats {
    std::size_t rules = 0;
    std::size_t declarations = 0;
    std::size_t componentValues = 0;
    bool rejected = false;
  };

  /// Maximum declarations retained across one stylesheet.
  static constexpr std::size_t kMaximumDeclarations = 16 * 1024;

  /// Default constructor.
  Stylesheet() = default;

  /**
   * Construct a stylesheet from a list of rules.
   *
   * @param rules List of rules, ownership is taken.
   * @param fontFaces Optional list of `@font-face` declarations, ownership is taken.
   */
  explicit Stylesheet(std::vector<SelectorRule>&& rules, std::vector<FontFace>&& fontFaces = {})
      : Stylesheet(std::move(rules), std::move(fontFaces), SecurityStats{}) {}

  /** Construct a stylesheet with parser resource-limit accounting. */
  Stylesheet(std::vector<SelectorRule>&& rules, std::vector<FontFace>&& fontFaces,
             SecurityStats securityStats)
      : rules_(std::move(rules)), fontFaces_(std::move(fontFaces)), securityStats_(securityStats) {}

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
  std::span<const SelectorRule> rules() const UTILS_LIFETIME_BOUND { return rules_; }

  /**
   * Get the list of `@font-face` rules in this stylesheet.
   */
  std::span<const FontFace> fontFaces() const UTILS_LIFETIME_BOUND { return fontFaces_; }

  /// Return resource-limit accounting from parsing this stylesheet.
  const SecurityStats& securityStats() const { return securityStats_; }

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
  SecurityStats securityStats_;
};

}  // namespace donner::css
