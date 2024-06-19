#pragma once
/// @file

#include "donner/base/Length.h"
#include "donner/svg/SVGGeometryElement.h"

namespace donner::svg {

/**
 * @page xml_circle '<circle>'
 * @ingroup elements_basic_shapes
 *
 * Creates a circle centered on `cx`, `cy`, with radius `r`.
 *
 * - DOM object: SVGCircleElement
 * - SVG2 spec: https://www.w3.org/TR/SVG2/shapes.html#CircleElement
 *
 * \htmlonly
 * <svg id="xml_circle" width="300" height="300" style="background-color: white">
 *   <style>
 *     #xml_circle text { font-size: 16px; font-weight: bold; color: black }
 *     #xml_circle line { stroke: black; stroke-width: 2px; stroke-dasharray: 6,4 }
 *     #xml_circle circle { stroke-width: 2px }
 *   </style>
 *
 *   <circle cx="150" cy="150" r="140" fill="none" stroke="black" />
 *   <circle cx="150" cy="150" r="3" fill="black" />
 *   <text x="160" y="153">cx,cy</text>
 *   <line x1="150" y1="150" x2="150" y2="10" stroke="black" />
 *   <text x="160" y="80">r</text>
 * </svg>
 * \endhtmlonly
 *
 * | Attribute | Default | Description  |
 * | --------: | :-----: | :----------- |
 * | `cx`      | `0`     | Center X coordinate. |
 * | `cy`      | `0`     | Center Y coordinate. |
 * | `r`       | `0`     | Radius of the circle. |
 */

/**
 * DOM object for a \ref xml_circle element.
 *
 * Use the `cx`, `cy`, and `r` attributes to define the circle.
 *
 * \htmlonly
 * <svg id="xml_circle" width="300" height="300" style="background-color: white">
 *   <style>
 *     #xml_circle text { font-size: 16px; font-weight: bold; color: black }
 *     #xml_circle line { stroke: black; stroke-width: 2px; stroke-dasharray: 6,4 }
 *     #xml_circle circle { stroke-width: 2px }
 *   </style>
 *
 *   <circle cx="150" cy="150" r="140" fill="none" stroke="black" />
 *   <circle cx="150" cy="150" r="3" fill="black" />
 *   <text x="160" y="153">cx,cy</text>
 *   <line x1="150" y1="150" x2="150" y2="10" stroke="black" />
 *   <text x="160" y="80">r</text>
 * </svg>
 * \endhtmlonly
 */
class SVGCircleElement : public SVGGeometryElement {
private:
  /// Create an SVGCircleElement wrapper from an entity.
  explicit SVGCircleElement(EntityHandle handle) : SVGGeometryElement(handle) {}

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::Circle;
  /// XML tag name, \ref xml_circle.
  static constexpr std::string_view Tag{"circle"};

  /**
   * Create a new \ref xml_circle element.
   *
   * @param document Containing document.
   */
  static SVGCircleElement Create(SVGDocument& document);

  /**
   * Set the center X coordinate.
   *
   * @param value Coordinate value.
   */
  void setCx(Lengthd value);

  /**
   * Set the center Y coordinate.
   *
   * @param value Coordinate value.
   */
  void setCy(Lengthd value);

  /**
   * Set the radius.
   *
   * @param value Radius value.
   */
  void setR(Lengthd value);

  /**
   * Get the center X coordinate.
   */
  Lengthd cx() const;

  /**
   * Get the center Y coordinate.
   */
  Lengthd cy() const;

  /**
   * Get the radius.
   */
  Lengthd r() const;

  /**
   * Applies stylesheet rules to the element, and returns the computed value of the `cx` property.
   */
  Lengthd computedCx() const;

  /**
   * Applies stylesheet rules to the element, and returns the computed value of the `cy` property.
   */
  Lengthd computedCy() const;

  /**
   * Applies stylesheet rules to the element, and returns the computed value of the `r` property.
   */
  Lengthd computedR() const;

private:
  /// Invalidates cached data from the render tree.
  void invalidate() const;
  /// Create the computed path data for this circle, to be used for rendering.
  void compute() const;
};

}  // namespace donner::svg
