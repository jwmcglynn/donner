#pragma once
/// @file

#include <span>

#include "donner/base/parser/ParseResult.h"
#include "donner/css/ComponentValue.h"
#include "donner/svg/core/CssTransform.h"

namespace donner::svg::parser {

/**
 * Parse a CSS "transform" property, see https://www.w3.org/TR/css-transforms-1/#transform-property
 */
class CssTransformParser {
public:
  /**
   * Parse a CSS "transform" property, see
   * https://www.w3.org/TR/css-transforms-1/#transform-property
   *
   * @param components CSS ComponentValues for a parsed transform property.
   * @return Parsed CSS transform, or an error.
   */
  static ParseResult<CssTransform> Parse(std::span<const css::ComponentValue> components);
};

}  // namespace donner::svg::parser
