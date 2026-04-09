#pragma once
/// @file

#include "donner/svg/SVGFilterPrimitiveStandardAttributes.h"

namespace donner::svg {

/**
 * @page xml_feDisplacementMap "<feDisplacementMap>"
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
 *
 * \htmlonly
 * <svg id="xml_feDisplacementMap_diagram" width="320" height="160" viewBox="0 0 320 160" style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <filter id="xml_feDisplacementMap_filter" x="-10%" y="-10%" width="120%" height="120%">
 *       <feTurbulence type="turbulence" baseFrequency="0.05" numOctaves="2" result="xml_feDisplacementMap_noise" />
 *       <feDisplacementMap in="SourceGraphic" in2="xml_feDisplacementMap_noise" scale="20" />
 *     </filter>
 *   </defs>
 *   <rect x="20" y="30" width="100" height="70" fill="#3498db" />
 *   <text x="20" y="120" fill="black">Source</text>
 *   <rect x="180" y="30" width="100" height="70" fill="#3498db" filter="url(#xml_feDisplacementMap_filter)" />
 *   <text x="180" y="120" fill="black">scale="20"</text>
 * </svg>
 * \endhtmlonly
 *
 * | Attribute    | Default | Description  |
 * | -----------: | :-----: | :----------- |
 * | `scale`      | `0`     | Maximum displacement in user units. |
 * | `xChannelSelector` | `A` | Channel (`R`, `G`, `B`, `A`) of `in2` used for x displacement. |
 * | `yChannelSelector` | `A` | Channel (`R`, `G`, `B`, `A`) of `in2` used for y displacement. |
 *
 * Inherits standard filter primitive attributes (`in`, `in2`, `result`, `x`, `y`, `width`,
 * `height`) from \ref donner::svg::SVGFilterPrimitiveStandardAttributes.
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
};

}  // namespace donner::svg
