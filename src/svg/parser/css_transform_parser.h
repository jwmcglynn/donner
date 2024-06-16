#pragma once
/// @file

#include <span>

#include "src/base/parser/parse_result.h"
#include "src/css/component_value.h"
#include "src/svg/core/css_transform.h"

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
