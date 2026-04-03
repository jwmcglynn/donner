#pragma once
/// @file

#include "donner/svg/SVGFilterPrimitiveStandardAttributes.h"

namespace donner::svg {

/**
 * @defgroup xml_feBlend "<feBlend>"
 *
 * Defines a filter primitive that composites two input images using blend modes.
 *
 * - DOM object: SVGFEBlendElement
 * - SVG2 spec: https://www.w3.org/TR/filter-effects/#feBlendElement
 *
 * Example usage:
 *
 * ```xml
 * <filter id="Multiply">
 *   <feFlood flood-color="red" result="red" />
 *   <feBlend in="SourceGraphic" in2="red" mode="multiply" />
 * </filter>
 * ```
 */

/**
 * DOM object for a \ref xml_feBlend element.
 *
 * Composites input image `in` over `in2` using a CSS blend mode.
 */
class SVGFEBlendElement : public SVGFilterPrimitiveStandardAttributes {
  friend class parser::SVGParserImpl;

protected:
  explicit SVGFEBlendElement(EntityHandle handle)
      : SVGFilterPrimitiveStandardAttributes(handle) {}

  static SVGFEBlendElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::FeBlend;
  /// XML tag name, \ref xml_feBlend.
  static constexpr std::string_view Tag{"feBlend"};
  /// This is an experimental/incomplete feature.
  static constexpr bool IsExperimental = true;
};

}  // namespace donner::svg
