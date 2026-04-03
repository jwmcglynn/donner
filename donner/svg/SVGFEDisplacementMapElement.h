#pragma once
/// @file

#include "donner/svg/SVGFilterPrimitiveStandardAttributes.h"

namespace donner::svg {

/**
 * @defgroup xml_feDisplacementMap "<feDisplacementMap>"
 *
 * Uses the pixel values from a second input to spatially displace the first input image.
 *
 * - DOM object: SVGFEDisplacementMapElement
 * - SVG2 spec: https://www.w3.org/TR/filter-effects/#feDisplacementMapElement
 *
 * This element is defined within a \ref xml_filter element, and is combined with other filter
 * primitives to define a filter applied on the input image.
 *
 * Example usage:
 *
 * ```xml
 * <filter id="MyFilter">
 *   <feTurbulence type="turbulence" baseFrequency="0.05" result="noise" />
 *   <feDisplacementMap in="SourceGraphic" in2="noise" scale="20" />
 * </filter>
 * ```
 */

/**
 * DOM object for a \ref xml_feDisplacementMap element.
 *
 * Spatially displaces pixels from the first input using channel values from the second input
 * as a displacement map. The displacement at each pixel is:
 * `dst[x,y] = src[x + scale*(map[x,y].xChannel/255 - 0.5), y + scale*(map[x,y].yChannel/255 -
 * 0.5)]`
 */
class SVGFEDisplacementMapElement : public SVGFilterPrimitiveStandardAttributes {
  friend class parser::SVGParserImpl;

protected:
  /// Create an SVGFEDisplacementMapElement wrapper from an entity.
  explicit SVGFEDisplacementMapElement(EntityHandle handle)
      : SVGFilterPrimitiveStandardAttributes(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGFEDisplacementMapElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::FeDisplacementMap;
  /// XML tag name, \ref xml_feDisplacementMap.
  static constexpr std::string_view Tag{"feDisplacementMap"};
  /// This is an experimental/incomplete feature.
  static constexpr bool IsExperimental = true;
};

}  // namespace donner::svg
