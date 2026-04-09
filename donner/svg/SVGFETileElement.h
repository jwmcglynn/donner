#pragma once
/// @file

#include "donner/svg/SVGFilterPrimitiveStandardAttributes.h"

namespace donner::svg {

/**
 * @page xml_feTile "<feTile>"
 *
 * Defines a filter primitive that tiles the input image across the output region.
 *
 * - DOM object: SVGFETileElement
 * - SVG2 spec: https://www.w3.org/TR/filter-effects/#feTileElement
 *
 * This element is defined within a \ref xml_filter element, and is combined with other filter
 * primitives to define a filter applied on the input image.
 *
 * Example usage:
 *
 * ```xml
 * <filter id="MyFilter">
 *   <feFlood flood-color="seagreen" x="28" y="28" width="10" height="10"/>
 *   <feOffset dx="5" dy="5"/>
 *   <feTile/>
 * </filter>
 * ```
 *
 * \htmlonly
 * <svg id="xml_feTile_diagram" width="320" height="160" viewBox="0 0 320 160" style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <pattern id="xml_feTile_pat" x="0" y="0" width="20" height="20" patternUnits="userSpaceOnUse">
 *       <rect width="20" height="20" fill="#ecf0f1" />
 *       <circle cx="10" cy="10" r="6" fill="#e74c3c" />
 *     </pattern>
 *   </defs>
 *   <rect x="30" y="40" width="20" height="20" fill="#ecf0f1" />
 *   <circle cx="40" cy="50" r="6" fill="#e74c3c" />
 *   <text x="20" y="85" fill="black">Source tile</text>
 *   <rect x="140" y="30" width="160" height="80" fill="url(#xml_feTile_pat)" stroke="#888" />
 *   <text x="170" y="130" fill="black">feTile output region</text>
 * </svg>
 * \endhtmlonly
 *
 * This element takes no attributes of its own.
 *
 * Inherits standard filter primitive attributes (`in`, `result`, `x`, `y`, `width`, `height`)
 * from \ref donner::svg::SVGFilterPrimitiveStandardAttributes.
 */

/**
 * DOM object for a \ref xml_feTile element.
 *
 * Fills a target rectangle with a repeated, tiled pattern of the input image. The tile is
 * defined by the content within the input filter primitive's primitive subregion.
 */
class SVGFETileElement : public SVGFilterPrimitiveStandardAttributes {
  friend class parser::SVGParserImpl;

protected:
  /// Create an SVGFETileElement wrapper from an entity.
  explicit SVGFETileElement(EntityHandle handle) : SVGFilterPrimitiveStandardAttributes(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGFETileElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::FeTile;
  /// XML tag name, \ref xml_feTile.
  static constexpr std::string_view Tag{"feTile"};
};

}  // namespace donner::svg
