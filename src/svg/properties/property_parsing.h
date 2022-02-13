#pragma once

#include "src/css/specificity.h"
#include "src/svg/parser/length_percentage_parser.h"
#include "src/svg/properties/property.h"

namespace donner::svg {

struct PropertyParseFnParams {
  std::variant<std::string_view, std::span<const css::ComponentValue>> valueOrComponents;
  PropertyState explicitState = PropertyState::NotSet;
  css::Specificity specificity;
  /// For presentation attributes, values may be unitless, in which case they the spec says they are
  /// specified in "user units". See https://www.w3.org/TR/SVG2/types.html#syntax.
  bool allowUserUnits = false;

  std::span<const css::ComponentValue> components() const;

private:
  mutable std::optional<std::vector<css::ComponentValue>> parsedComponents_;
};

PropertyParseFnParams CreateParseFnParams(const css::Declaration& declaration,
                                          css::Specificity specificity);

/**
 * If the components contain only a single ident, returns an RcString for that ident's contents.
 *
 * @param components Component values, which should already be trimmed.
 * @return A string if the components contain a single ident, otherwise std::nullopt.
 */
std::optional<RcString> TryGetSingleIdent(std::span<const css::ComponentValue> components);

/**
 * Parse a <length-percentage>, which may optionally be set to "auto", in which case this returns
 * std::nullopt.
 *
 * @param components Component values, which should already be trimmed.
 * @param allowUserUnits Whether to allow unitless values, if this is a parse in the context of XML
 *   attributes.
 * @return Return an optional, which is set to std::nullopt if the value is "auto", or a Length, or
 *   a parse error.
 */
ParseResult<std::optional<Lengthd>> ParseLengthPercentageOrAuto(
    std::span<const css::ComponentValue> components, bool allowUserUnits);

}  // namespace donner::svg
