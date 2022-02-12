#pragma once

#include "src/css/specificity.h"
#include "src/svg/properties/property.h"

namespace donner::svg {

struct PropertyParseFnParams {
  std::span<const css::ComponentValue> components;
  PropertyState explicitState = PropertyState::NotSet;
  css::Specificity specificity;
  /// For presentation attributes, values may be unitless, in which case they the spec says they are
  /// specified in "user units". See https://www.w3.org/TR/SVG2/types.html#syntax.
  bool allowUserUnits = false;
};

ParseResult<Lengthd> ParseLengthPercentage(std::span<const css::ComponentValue> components,
                                           bool allowUserUnits);

}  // namespace donner::svg
