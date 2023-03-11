#pragma once
/// @file

#include "src/base/length.h"
#include "src/svg/svg_geometry_element.h"

namespace donner::svg {

/**
 * @defgroup xml_ellipse `<ellipse>'
 *
 * Creates an ellipse centered on `cx`, `cy`, with radius `rx` and `ry`.
 *
 * - DOM object: SVGEllipseElement
 * - SVG2 spec: https://www.w3.org/TR/SVG2/shapes.html#EllipseElement
 *
 * ```xml
 * <ellipse cx="150" cy="150" rx="140" ry="100" fill="none" stroke="black" />
 * ```
 *
 * \htmlonly
 * <svg width="300" height="300" style="background-color: white">
 *   <style>
 *     text { font-size: 16px; font-weight: bold; color: black }
 *     line { stroke: black; stroke-width: 2px; stroke-dasharray: 6,4 }
 *     ellipse { stroke-width: 2px }
 *   </style>
 *
 *   <ellipse cx="150" cy="150" rx="140" ry="100" fill="none" stroke="black" />
 *   <circle cx="150" cy="150" r="3" fill="black" />
 *   <text x="160" y="153">cx,cy</text>
 *   <line x1="150" y1="150" x2="10" y2="150" stroke="black" />
 *   <text x="75" y="170">rx</text>
 *   <line x1="150" y1="150" x2="150" y2="50" stroke="black" />
 *   <text x="160" y="100">ry</text>
 * </svg>
 * \endhtmlonly
 */

/**
 * DOM object for a \ref xml_ellipse element.
 *
 * Use the `cx`, `cy`, `rx`, and `ry` attributes to define the ellipse.
 *
 * \htmlonly
 * <svg width="300" height="300" style="background-color: white">
 *   <style>
 *     text { font-size: 16px; font-weight: bold; color: black }
 *     line { stroke: black; stroke-width: 2px; stroke-dasharray: 6,4 }
 *     ellipse { stroke-width: 2px }
 *   </style>
 *
 *   <ellipse cx="150" cy="150" rx="140" ry="100" fill="none" stroke="black" />
 *   <circle cx="150" cy="150" r="3" fill="black" />
 *   <text x="160" y="153">cx,cy</text>
 *   <line x1="150" y1="150" x2="10" y2="150" stroke="black" />
 *   <text x="75" y="170">rx</text>
 *   <line x1="150" y1="150" x2="150" y2="50" stroke="black" />
 *   <text x="160" y="100">ry</text>
 * </svg>
 * \endhtmlonly
 *
 * | Attribute | Default | Description  |
 * | --------: | :-----: | :----------- |
 * | `cx`      | `0`     | Center X coordinate. |
 * | `cy`      | `0`     | Center Y coordinate. |
 * | `rx`      | `auto` (\ref xy_auto) | Horizontal radius, along the X axis. |
 * | `ry`      | `auto` (\ref xy_auto) | Vertical radius, along the Y axis. |
 */
class SVGEllipseElement : public SVGGeometryElement {
private:
  /// Create an SVGEllipseElement wrapper from an entity.
  explicit SVGEllipseElement(EntityHandle handle) : SVGGeometryElement(handle) {}

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::Ellipse;
  /// XML tag name, \ref xml_ellipse.
  static constexpr std::string_view Tag = "ellipse";

  /**
   * Create a new \ref xml_ellipse element.
   *
   * @param document Containing document.
   */
  static SVGEllipseElement Create(SVGDocument& document);

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
   * Set the horizontal radius, along the X axis.
   *
   * @param value Radius value.
   * @param value Radius value, or `std::nullopt` for `auto`, to use the same value as `ry` (see
   * \ref xy_auto).
   */
  void setRx(std::optional<Lengthd> value);

  /**
   * Set the vertical radius, along the Y axis.
   *
   * @param value Radius value, or `std::nullopt` for `auto`, to use the same value as `rx` (see
   * \ref xy_auto).
   */
  void setRy(std::optional<Lengthd> value);

  /**
   * Get the center X coordinate.
   *
   * @return Coordinate value.
   */
  Lengthd cx() const;

  /**
   * Get the center Y coordinate.
   *
   * @return Coordinate value.
   */
  Lengthd cy() const;

  /**
   * Get the horizontal radius, along the X axis.
   *
   * @return Radius value, or `std::nullopt` for `auto`. To get the computed size, use \ref
   * computedRx.
   */
  std::optional<Lengthd> rx() const;

  /**
   * Get the vertical radius, along the Y axis.
   *
   * @return Radius value, or `std::nullopt` for `auto`. To get the computed size, use \ref
   * computedRy.
   */
  std::optional<Lengthd> ry() const;

  /**
   * Get the computed center X coordinate.
   *
   * @return Coordinate value.
   */
  Lengthd computedCx() const;

  /**
   * Get the computed center Y coordinate.
   *
   * @return Coordinate value.
   */
  Lengthd computedCy() const;

  /**
   * Get the computed horizontal radius, along the X axis.
   *
   * @return Radius value.
   */
  Lengthd computedRx() const;

  /**
   * Get the computed vertical radius, along the Y axis.
   *
   * @return Radius value.
   */
  Lengthd computedRy() const;

private:
  void invalidate() const;
  void compute() const;
};

}  // namespace donner::svg
