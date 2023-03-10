#pragma once
/// @file

#include "src/base/length.h"
#include "src/svg/svg_geometry_element.h"

namespace donner::svg {

/**
 * @defgroup circle <circle>
 *
 * Creates a circle centered on `cx`, `cy`, with radius `r`. See SVGCircleElement.
 *
 * Spec: https://www.w3.org/TR/SVG2/shapes.html#CircleElement
 *
 * \htmlonly
 * <svg width="300" height="300" style="background-color: white">
 *   <style>
 *     text { font-size: 16px; font-weight: bold; color: black }
 *     line { stroke: black; stroke-width: 2px; stroke-dasharray: 6,4 }
 *     circle { stroke-width: 2px }
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

/**
 * DOM object for a \ref circle element.
 *
 * Use the `cx`, `cy`, and `r` attributes to define the circle.
 *
 * \htmlonly
 * <svg width="300" height="300" style="background-color: white">
 *   <style>
 *     text { font-size: 16px; font-weight: bold; color: black }
 *     line { stroke: black; stroke-width: 2px; stroke-dasharray: 6,4 }
 *     circle { stroke-width: 2px }
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
class SVGCircleElement : public SVGGeometryElement {
private:
  /// Create an SVGCircleElement wrapper from an entity.
  explicit SVGCircleElement(EntityHandle handle) : SVGGeometryElement(handle) {}

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::Circle;
  /// XML tag name, \ref circle.
  static constexpr std::string_view Tag = "circle";

  /**
   * Create a new \ref circle element.
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
