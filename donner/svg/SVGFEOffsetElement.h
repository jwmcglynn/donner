#pragma once
/// @file

#include "donner/svg/SVGFilterPrimitiveStandardAttributes.h"

namespace donner::svg {

/**
 * @page xml_feOffset "<feOffset>"
 *
 * Defines a filter primitive that offsets the input image by (dx, dy).
 *
 * - DOM object: SVGFEOffsetElement
 * - SVG2 spec: https://www.w3.org/TR/filter-effects/#feOffsetElement
 *
 * This element is defined within a \ref xml_filter element, and is combined with other filter
 * primitives to define a filter applied on the input image.
 *
 * Example usage:
 *
 * ```xml
 * <filter id="MyFilter">
 *   <feOffset in="SourceGraphic" dx="10" dy="10" />
 * </filter>
 * ```
 *
 * \htmlonly
 * <svg id="xml_feOffset_diagram" width="320" height="160" viewBox="0 0 320 160" style="background-color: white" font-family="sans-serif" font-size="12">
 *   <defs>
 *     <filter id="xml_feOffset_filter" x="-20%" y="-20%" width="160%" height="160%">
 *       <feOffset in="SourceGraphic" dx="12" dy="8" />
 *     </filter>
 *   </defs>
 *   <rect x="30" y="30" width="70" height="60" fill="#4a90e2" />
 *   <text x="30" y="110" fill="black">Source</text>
 *   <rect x="170" y="30" width="70" height="60" fill="#4a90e2" fill-opacity="0.25" stroke="#4a90e2" stroke-dasharray="3,2" />
 *   <rect x="170" y="30" width="70" height="60" fill="#4a90e2" filter="url(#xml_feOffset_filter)" />
 *   <text x="170" y="130" fill="black">dx=12 dy=8</text>
 * </svg>
 * \endhtmlonly
 *
 * | Attribute | Default | Description  |
 * | --------: | :-----: | :----------- |
 * | `dx`      | `0`     | Horizontal offset in user units. |
 * | `dy`      | `0`     | Vertical offset in user units. |
 *
 * Inherits standard filter primitive attributes (`in`, `result`, `x`, `y`, `width`, `height`)
 * from \ref donner::svg::SVGFilterPrimitiveStandardAttributes.
 */

/**
 * DOM object for a \ref xml_feOffset element.
 *
 * Offsets the input image by (dx, dy). Pixels that are shifted outside the filter region are
 * discarded; uncovered areas become transparent black.
 */
class SVGFEOffsetElement : public SVGFilterPrimitiveStandardAttributes {
  friend class parser::SVGParserImpl;

protected:
  /// Create an SVGFEOffsetElement wrapper from an entity.
  explicit SVGFEOffsetElement(EntityHandle handle)
      : SVGFilterPrimitiveStandardAttributes(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGFEOffsetElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::FeOffset;
  /// XML tag name, \ref xml_feOffset.
  static constexpr std::string_view Tag{"feOffset"};

  /// Get the horizontal offset.
  double dx() const;

  /// Get the vertical offset.
  double dy() const;

  /**
   * Set the offset.
   *
   * @param dx Horizontal offset in user units.
   * @param dy Vertical offset in user units.
   */
  void setOffset(double dx, double dy);
};

}  // namespace donner::svg
