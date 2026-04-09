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
 *
 * \htmlonly
 * <svg id="xml_feMorphology_diagram" width="320" height="160" viewBox="0 0 320 160" style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <filter id="xml_feMorphology_dilate" x="-20%" y="-20%" width="140%" height="140%">
 *       <feMorphology operator="dilate" radius="3" />
 *     </filter>
 *     <filter id="xml_feMorphology_erode" x="-20%" y="-20%" width="140%" height="140%">
 *       <feMorphology operator="erode" radius="2" />
 *     </filter>
 *   </defs>
 *   <rect x="20" y="30" width="60" height="60" rx="10" fill="#4a90e2" />
 *   <text x="20" y="110" fill="black">Source</text>
 *   <rect x="120" y="30" width="60" height="60" rx="10" fill="#4a90e2" filter="url(#xml_feMorphology_dilate)" />
 *   <text x="115" y="110" fill="black">dilate r=3</text>
 *   <rect x="220" y="30" width="60" height="60" rx="10" fill="#4a90e2" filter="url(#xml_feMorphology_erode)" />
 *   <text x="220" y="110" fill="black">erode r=2</text>
 * </svg>
 * \endhtmlonly
 *
 * | Attribute  | Default  | Description  |
 * | ---------: | :------: | :----------- |
 * | `operator` | `erode`  | `erode` (minimum) or `dilate` (maximum). |
 * | `radius`   | `0`      | Kernel radius; may be one or two numbers (x, y). |
 *
 * Inherits standard filter primitive attributes (`in`, `result`, `x`, `y`, `width`, `height`)
 * from \ref donner::svg::SVGFilterPrimitiveStandardAttributes.
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
