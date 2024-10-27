#pragma once
/// @file

#include "donner/svg/SVGElement.h"
#include "donner/svg/components/filter/FilterUnits.h"

namespace donner::svg {

/**
 * @defgroup xml_filter "<filter>"
 *
 * Defines filter effects which can be applied to graphical elements.
 *
 * - DOM object: SVGFilterElement
 * - SVG2 spec: https://www.w3.org/TR/filter-effects/#FilterElement
 *
 * These elements are typically placed within a `<defs>` element, and then referenced by id from a
 * `filter` attribute.
 *
 * Inside the `<filter>` element there may be any number of filter primitive elements, such as
 * `<feGaussianBlur>`.
 *
 * Example usage:
 *
 * ```xml
 * <filter id="MyFilter">
 *   <feGaussianBlur in="SourceGraphic" stdDeviation="5" />
 * </filter>
 * ```
 *
 * To reference it with the filter attribute:
 * ```xml
 * <rect filter="url(#MyFilter)" width="300" height="300" />
 * ```
 */

/**
 * DOM object for a \ref xml_filter element.
 *
 * ```xml
 * <filter id="MyFilter">
 *   <feGaussianBlur in="SourceGraphic" stdDeviation="5" />
 * </filter>
 * ```
 *
 * To reference it with a filter:
 * ```xml
 * <rect filter="url(#MyFilter)" width="300" height="300" />
 * ```
 */
class SVGFilterElement : public SVGElement {
  friend class parser::SVGParserImpl;

protected:
  /// Create an SVGFilterElement wrapper from an entity.
  explicit SVGFilterElement(EntityHandle handle) : SVGElement(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGFilterElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::Filter;
  /// XML tag name, \ref xml_filter.
  static constexpr std::string_view Tag{"filter"};
  /// This is an experimental/incomplete feature.
  static constexpr bool IsExperimental = true;

  /**
   * Create a new \ref xml_filter element.
   *
   * @param document Containing document.
   */
  static SVGFilterElement Create(SVGDocument& document) { return CreateOn(CreateEntity(document)); }

  /**
   * Get the top-left X coordinate of the filter region, which defines a rectangular region on the
   * canvas to which this filter applies. The initial value is '-10%'.
   */
  Lengthd x() const;

  /**
   * Get the top-left Y coordinate of the filter region, which defines a rectangular region on the
   * canvas to which this filter applies. The initial value is '-10%'.
   */
  Lengthd y() const;

  /**
   * Get the width of the filter region, which defines a rectangular region on the
   * canvas to which this filter applies. The initial value is '120%'.
   */
  Lengthd width() const;

  /**
   * Get the height of the filter region, which defines a rectangular region on the
   * canvas to which this filter applies. The initial value is '120%'.
   */
  Lengthd height() const;

  /**
   * Set the top-left X coordinate of the filter region, which defines a rectangular region on the
   * canvas to which this filter applies. The initial value is '-10%'.
   *
   * @param value Coordinate value.
   */
  void setX(const Lengthd& value);

  /**
   * Set the top-left Y coordinate of the filter region, which defines a rectangular region on the
   * canvas to which this filter applies. The initial value is '-10%'.
   *
   * @param value Coordinate value.
   */
  void setY(const Lengthd& value);

  /**
   * Set the width of the filter region, which defines a rectangular region on the
   * canvas to which this filter applies. The initial value is '120%'.
   *
   * @param value Dimension value.
   */
  void setWidth(const Lengthd& value);

  /**
   * Set the height of the filter region, which defines a rectangular region on the
   * canvas to which this filter applies. The initial value is '120%'.
   *
   * @param value Dimension value.
   */
  void setHeight(const Lengthd& value);

  /**
   * Get the `filterUnits` attribute which defines the coordinate system for attributes `x`,
   * `y`, `width`, `height`.
   *
   * The default is \ref FilterUnits::ObjectBoundingBox, where (0, 0) is the top-left corner of the
   * element that references the filter, and (1, 1) is the bottom-right corner.
   */
  FilterUnits filterUnits() const;

  /**
   * Set the `filterUnits` attribute which defines the coordinate system for attributes `x`,
   * `y`, `width`, `height`.
   *
   * \see \ref filterUnits()
   *
   * @param value The coordinate system for attributes `x`, `y`, `width`, `height`.
   */
  void setFilterUnits(FilterUnits value);

  /**
   * Get the `primitiveUnits` attribute which defines the coordinate system for the various
   * length values within the filter primitives and for the attributes that define the filter
   * primitive subregion.
   *
   * The default is \ref FilterUnits::UserSpaceOnUse, where the user coordinate system in place
   * at the time when the `filter` element is referenced is used.
   */
  PrimitiveUnits primitiveUnits() const;

  /**
   * Set the `primitiveUnits` attribute which defines the coordinate system for the various
   * length values within the filter primitives and for the attributes that define the filter
   * primitive subregion.
   *
   * \see \ref primitiveUnits()
   *
   * @param value The coordinate system for the various length values within the filter primitives.
   */
  void setPrimitiveUnits(PrimitiveUnits value);
};

}  // namespace donner::svg
