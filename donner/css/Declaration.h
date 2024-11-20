#pragma once
/// @file

#include <ostream>
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
   * Output a human-readable representation of the declaration to a stream.
   *
   * @param os Output stream.
   * @param declaration Declaration to output.
   */
  friend std::ostream& operator<<(std::ostream& os, const Declaration& declaration);

  RcString name;                       ///< Name of the declaration.
  std::vector<ComponentValue> values;  ///< List of component values for the declaration.
  FileOffset sourceOffset;             ///< Offset of the declaration name in the source string.
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

}  // namespace donner::css
