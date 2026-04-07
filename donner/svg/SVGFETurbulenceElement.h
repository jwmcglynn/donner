#pragma once
/// @file

#include "donner/svg/SVGFilterPrimitiveStandardAttributes.h"

namespace donner::svg {

/**
 * @page xml_feTurbulence "<feTurbulence>"
 *
 * Generates Perlin noise or fractal noise to fill the filter primitive subregion.
 *
 * - DOM object: SVGFETurbulenceElement
 * - SVG2 spec: https://www.w3.org/TR/filter-effects/#feTurbulenceElement
 *
 * This element is defined within a \ref xml_filter element, and is combined with other filter
 * primitives to define a filter applied on the input image.
 *
 * Example usage:
 *
 * ```xml
 * <filter id="MyFilter">
 *   <feTurbulence type="turbulence" baseFrequency="0.05" numOctaves="3" />
 * </filter>
 * ```
 */

/**
 * DOM object for a \ref xml_feTurbulence element.
 *
 * Generates a procedural noise texture using the Perlin noise algorithm. Supports both
 * turbulence and fractalNoise types with configurable frequency, octave count, and seed.
 */
class SVGFETurbulenceElement : public SVGFilterPrimitiveStandardAttributes {
  friend class parser::SVGParserImpl;

protected:
  /// Create an SVGFETurbulenceElement wrapper from an entity.
  explicit SVGFETurbulenceElement(EntityHandle handle)
      : SVGFilterPrimitiveStandardAttributes(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGFETurbulenceElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::FeTurbulence;
  /// XML tag name, \ref xml_feTurbulence.
  static constexpr std::string_view Tag{"feTurbulence"};
};

}  // namespace donner::svg
