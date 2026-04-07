#pragma once
/// @file

#include "donner/svg/SVGFilterPrimitiveStandardAttributes.h"

namespace donner::svg {

/**
 * @page xml_feMorphology "<feMorphology>"
 *
 * Defines a filter primitive that erodes or dilates the input image.
 *
 * - DOM object: SVGFEMorphologyElement
 * - SVG2 spec: https://www.w3.org/TR/filter-effects/#feMorphologyElement
 *
 * This element is defined within a \ref xml_filter element, and is combined with other filter
 * primitives to define a filter applied on the input image.
 *
 * Example usage:
 *
 * ```xml
 * <filter id="MyFilter">
 *   <feMorphology in="SourceGraphic" operator="dilate" radius="5" />
 * </filter>
 * ```
 */

/**
 * DOM object for a \ref xml_feMorphology element.
 *
 * Erodes or dilates the input image by the specified radius. Erode computes the per-channel
 * minimum over a rectangular window; dilate computes the maximum.
 */
class SVGFEMorphologyElement : public SVGFilterPrimitiveStandardAttributes {
  friend class parser::SVGParserImpl;

protected:
  /// Create an SVGFEMorphologyElement wrapper from an entity.
  explicit SVGFEMorphologyElement(EntityHandle handle)
      : SVGFilterPrimitiveStandardAttributes(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGFEMorphologyElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::FeMorphology;
  /// XML tag name, \ref xml_feMorphology.
  static constexpr std::string_view Tag{"feMorphology"};
};

}  // namespace donner::svg
