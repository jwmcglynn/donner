#pragma once
/// @file

#include "donner/base/Length.h"
#include "donner/svg/SVGGeometryElement.h"

namespace donner::svg {

/**
 * @page xml_line "<line>"
 *
 * Creates a line between two points, using the `x1`, `y1`, `x2`, and `y2` attributes.
 *
 * - DOM object: SVGLineElement
 * - SVG2 spec: https://www.w3.org/TR/SVG2/shapes.html#LineElement
 *
 * ```xml
 * <line x1="100" y1="100" x2="200" y2="200" stroke="black" stroke-width="2" />
 * ```
 *
 * \htmlonly
 * <svg id="xml_line" width="300" height="300" style="background-color: white">
 *   <style>
 *     #xml_line text { font-size: 16px; font-weight: bold; color: black }
 *   </style>
 *   <line x1="100" y1="100" x2="200" y2="200" stroke="black" stroke-width="2" />
 *
 *   <circle cx="100" cy="100" r="3" fill="black" />
 *   <text x="110" y="103">x1,y1</text>
 *
 *   <circle cx="200" cy="200" r="3" fill="black" />
 *   <text x="210" y="203">x2,y2</text>
 * </svg>
 * \endhtmlonly
 *
 * | Attribute | Default | Description  |
 * | --------: | :-----: | :----------- |
 * | `x1`      | `0`     | Start X coordinate. |
 * | `y1`      | `0`     | Start Y coordinate. |
 * | `x2`      | `0`     | End X coordinate. |
 * | `y2`      | `0`     | End Y coordinate. |
 */

/**
 * DOM object for a \ref xml_line element.
 *
 * Use the `x1`, `y1`, `x2`, and `y2` attributes to define the start and end of the line.
 *
 * \htmlonly
 * <svg id="xml_line" width="300" height="300" style="background-color: white">
 *   <style>
 *     #xml_line text { font-size: 16px; font-weight: bold; color: black }
 *   </style>
 *   <line x1="100" y1="100" x2="200" y2="200" stroke="black" stroke-width="2" />
 *
 *   <circle cx="100" cy="100" r="3" fill="black" />
 *   <text x="110" y="103">x1,y1</text>
 *
 *   <circle cx="200" cy="200" r="3" fill="black" />
 *   <text x="210" y="203">x2,y2</text>
 * </svg>
 * \endhtmlonly
 */
class SVGLineElement : public SVGGeometryElement {
  friend class parser::SVGParserImpl;

protected:
  /// Create an SVGLineElement wrapper from an entity.
  explicit SVGLineElement(EntityHandle handle) : SVGGeometryElement(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGLineElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::Line;
  /// XML tag name, \ref xml_line.
  static constexpr std::string_view Tag{"line"};

  static_assert(SVGGeometryElement::IsBaseOf(Type));
  static_assert(SVGGraphicsElement::IsBaseOf(Type));

  /**
   * Create a new \ref xml_line element.
   *
   * @param document Containing document.
   */
  static SVGLineElement Create(SVGDocument& document) {
    return CreateOn(CreateEmptyEntity(document));
  }

  /**
   * Set the start X coordinate.
   *
   * @param value Coordinate value.
   */
  void setX1(Lengthd value);

  /**
   * Set the start Y coordinate.
   *
   * @param value Coordinate value.
   */
  void setY1(Lengthd value);

  /**
   * Set the end X coordinate.
   *
   * @param value Coordinate value.
   */
  void setX2(Lengthd value);

  /**
   * Set the end Y coordinate.
   *
   * @param value Coordinate value.
   */
  void setY2(Lengthd value);

  /**
   * Get the start X coordinate.
   */
  Lengthd x1() const;

  /**
   * Get the start Y coordinate.
   */
  Lengthd y1() const;

  /**
   * Get the end X coordinate.
   */
  Lengthd x2() const;

  /**
   * Get the end Y coordinate.
   */
  Lengthd y2() const;

private:
  /// Invalidates cached data from the render tree.
  void invalidate() const;
};

}  // namespace donner::svg
