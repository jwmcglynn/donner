#pragma once
/// @file

#include "donner/svg/SVGFilterPrimitiveStandardAttributes.h"

namespace donner::svg {

/**
 * @page xml_feComposite "<feComposite>"
 *
 * Defines a filter primitive that composites two input images using Porter-Duff operators or
 * arithmetic combination.
 *
 * - DOM object: SVGFECompositeElement
 * - SVG2 spec: https://www.w3.org/TR/filter-effects/#feCompositeElement
 *
 * Example usage:
 *
 * ```xml
 * <filter id="MyFilter">
 *   <feComposite in="SourceGraphic" in2="flood" operator="over" />
 * </filter>
 * ```
 */

/**
 * DOM object for a \ref xml_feComposite element.
 *
 * Composites `in` over `in2` using the specified Porter-Duff operator. The `arithmetic` operator
 * uses the formula: `result = k1*in*in2 + k2*in + k3*in2 + k4`.
 */
class SVGFECompositeElement : public SVGFilterPrimitiveStandardAttributes {
  friend class parser::SVGParserImpl;

protected:
  explicit SVGFECompositeElement(EntityHandle handle)
      : SVGFilterPrimitiveStandardAttributes(handle) {}

  static SVGFECompositeElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::FeComposite;
  /// XML tag name, \ref xml_feComposite.
  static constexpr std::string_view Tag{"feComposite"};
};

}  // namespace donner::svg
