#pragma once
/// @file

#include "donner/svg/SVGFilterPrimitiveStandardAttributes.h"

namespace donner::svg {

/**
 * @page xml_feFlood "<feFlood>"
 *
 * Defines a filter primitive that fills the filter subregion with a solid color and opacity.
 *
 * - DOM object: SVGFEFloodElement
 * - SVG2 spec: https://www.w3.org/TR/filter-effects/#feFloodElement
 *
 * This element is defined within a \ref xml_filter element, and is combined with other filter
 * primitives to define a filter applied on the input image.
 *
 * Example usage:
 *
 * ```xml
 * <filter id="MyFilter">
 *   <feFlood flood-color="red" flood-opacity="0.5" result="flood" />
 *   <feMerge>
 *     <feMergeNode in="flood" />
 *     <feMergeNode in="SourceGraphic" />
 *   </feMerge>
 * </filter>
 * ```
 */

/**
 * DOM object for a \ref xml_feFlood element.
 *
 * Fills the filter primitive subregion with a solid color specified by `flood-color` and
 * `flood-opacity`. This primitive does not use any input image.
 */
class SVGFEFloodElement : public SVGFilterPrimitiveStandardAttributes {
  friend class parser::SVGParserImpl;

protected:
  /// Create an SVGFEFloodElement wrapper from an entity.
  explicit SVGFEFloodElement(EntityHandle handle)
      : SVGFilterPrimitiveStandardAttributes(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGFEFloodElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::FeFlood;
  /// XML tag name, \ref xml_feFlood.
  static constexpr std::string_view Tag{"feFlood"};
};

}  // namespace donner::svg
