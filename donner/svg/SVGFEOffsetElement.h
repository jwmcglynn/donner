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
