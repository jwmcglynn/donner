#pragma once
/// @file

#include <ostream>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "donner/base/RcString.h"
#include "donner/css/Rule.h"

namespace donner::css {

/**
 * A declaration is a CSS name/value pair, such as `color: red;`.
 *
 * The name is a CSS identifier, and the value is a list of component values which can be parsed
 * into higher-level constructs, such as a transform: `transform: translate(10px, 20px);`.
 *
 * The `important` flag is set if the declaration ends with `!important`, and the tokens for
 * `!important` are not included in the `values` list.
 */
struct Declaration {
  /**
   * Construct a new Declaration object.
   *
   * @param name Name of the declaration.
   * @param values List of component values for the declaration.
   * @param sourceOffset Offset of the declaration name in the source string.
   * @param important Whether the declaration ends with `!important`.
   */
  Declaration(RcString name, std::vector<ComponentValue> values = {},
              const FileOffset& sourceOffset = FileOffset::Offset(0), bool important = false)
      : name(std::move(name)),
        values(std::move(values)),
        sourceOffset(sourceOffset),
        sourceRange{sourceOffset, sourceOffset},
        important(important) {}

  /// Destructor.
  ~Declaration() = default;

  // Copy and move constructors and assignment operators.
  /// Copy constructor.
  Declaration(const Declaration& other) = default;
  /// Move constructor.
  Declaration(Declaration&& other) noexcept = default;
  /// Copy assignment operator.
  Declaration& operator=(const Declaration& other) = default;
  /// Move assignment operator.
  Declaration& operator=(Declaration&& other) noexcept = default;

  /// Equality operator.
  bool operator==(const Declaration& other) const = default;

  /**
   * Serialize this declaration back to its CSS text representation, e.g. `fill: red`.
   */
  std::string toCssText() const;

  /**
   * Output a human-readable representation of the declaration to a stream.
   *
   * @param os Output stream.
   * @param declaration Declaration to output.
   */
  friend std::ostream& operator<<(std::ostream& os, const Declaration& declaration);

  RcString name;                       ///< Name of the declaration.
  std::vector<ComponentValue> values;  ///< List of component values for the declaration.
  FileOffset sourceOffset;             ///< Offset of the declaration name in the source string.

  /**
   * Source byte range of the declaration in the stylesheet or `style=""` attribute, from the
   * first byte of the name to the offset *of* the last consumed value token.
   *
   * For `fill: red`, `sourceRange.start` points at `f` and `sourceRange.end` points at `r`
   * (the start of the last value token, not past its last byte). This is deliberately a
   * best-effort approximation: structured-editing callers that need a byte-perfect end to
   * splice into a `style=""` value can compute the trailing bound from the source text by
   * scanning forward from `sourceRange.end` to the `;`, closing brace, or end-of-input.
   *
   * `sourceRange.start == sourceRange.end` means "no consumed value tokens" — either the
   * parser failed partway, or the caller constructed the `Declaration` directly without
   * going through `DeclarationListParser`.
   */
  SourceRange sourceRange;
  bool important = false;              ///< Whether the declaration ends with `!important`.
};

/**
 * Return value of parsers that may return either a declaration or an AtRule, specifically \ref
 * donner::css::parser::DeclarationListParser::Parse.
 */
struct DeclarationOrAtRule {
  /**
   * The value of a DeclarationOrAtRule is either a Declaration, an AtRule, or an InvalidRule, and
   * is stored within this variant.
   */
  using Type = std::variant<Declaration, AtRule, InvalidRule>;

  /// The value of the DeclarationOrAtRule.
  Type value;

  /**
   * Construct a new DeclarationOrAtRule object with any of the \ref Type values.
   *
   * @param value Declaration value.
   */
  /* implicit */ DeclarationOrAtRule(Type&& value);

  /// Equality operator.
  bool operator==(const DeclarationOrAtRule& other) const;

  /**
   * Output a human-readable representation of the declaration or AtRule to a stream.
   *
   * @param os Output stream.
   * @param declOrAt DeclarationOrAtRule to output.
   */
  friend std::ostream& operator<<(std::ostream& os, const DeclarationOrAtRule& declOrAt) {
    std::visit([&os](auto&& v) { os << v; }, declOrAt.value);
    return os;
  }
};

/**
 * Merge two sets of CSS declarations, where \p updates override declarations in \p existing that
 * have the same property name (case-insensitive). Unrelated existing declarations are preserved.
 * Duplicate property names within \p updates are deduplicated, keeping the last occurrence.
 *
 * @param existing Existing declarations.
 * @param updates Updated declarations to apply.
 * @return Merged style string with declarations separated by "; ".
 */
std::string mergeStyleDeclarations(std::span<const Declaration> existing,
                                   std::span<const Declaration> updates);

}  // namespace donner::css
