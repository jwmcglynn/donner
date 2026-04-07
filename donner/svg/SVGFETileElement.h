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
