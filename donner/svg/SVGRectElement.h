#pragma once
/// @file

#include "donner/base/Length.h"
#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/core/PathSpline.h"

namespace donner::svg {

/**
 * @page xml_rect "<rect>"
 *
 * Creates a rectangle with the top-left corner at (`x`, `y`) and the specified `width` and
 * `height`, optionally with rounded corners.
 *
 * - DOM object: SVGRectElement
 * - SVG2 spec: https://www.w3.org/TR/SVG2/shapes.html#RectElement
 *
 * ```xml
 * <rect x="10" y="50" width="280" height="200" fill="none" stroke="black" />
 * ```
 *
 * \htmlonly
 * <svg id="xml_rect" width="300" height="300" style="background-color: white">
 *   <style>
 *     #xml_rect text { font-size: 16px; font-weight: bold; color: black }
 *     #xml_rect rect { stroke-width: 2px }
 *   </style>
 *
 *   <rect x="10" y="50" width="280" height="200" fill="none" stroke="black" />
 *   <circle cx="10" cy="50" r="3" fill="black" />
 *   <text x="20" y="70">x,y</text>
 *   <text x="140" y="40">width</text>
 *   <text x="20" y="150">height</text>
 * </svg>
 * \endhtmlonly
 *
 * ```xml
 * <rect x="10" y="50" width="280" height="200" rx="50" ry="40" fill="none" stroke="black" />
 * ```
 *
 * \htmlonly
 * <svg id="xml_rect_r" width="300" height="300" style="background-color: white">
 *   <style>
 *     #xml_rect_r text { font-size: 16px; font-weight: bold; color: black }
 *     #xml_rect_r line { stroke: black; stroke-width: 2px; stroke-dasharray: 6,4 }
 *     #xml_rect_r rect { stroke-width: 2px }
 *   </style>
 *
 *   <rect x="10" y="50" width="280" height="200" rx="100" ry="100" fill="none" stroke="black" />
 *   <line x1="10" y1="150" x2="110" y2="150" />
 *   <text x="50" y="170">rx</text>
 *   <line x1="110" y1="150" x2="110" y2="50" />
 *   <text x="120" y="100">ry</text>
 * </svg>
 * \endhtmlonly
 *
 * | Attribute | Default | Description  |
 * | --------: | :-----: | :----------- |
 * | `x`       | `0`     | Top-left corner X coordinate. |
 * | `y`       | `0`     | Top-left corner Y coordinate. |
 * | `width`   | `0`     | Horizontal radius, along the X axis. |
 * | `height`  | `0`     | Vertical radius, along the Y axis. |
 * | `rx`      | `auto` (\ref xy_auto) | For rounded corners, the radius of the curve along the X axis. |
 * | `ry`      | `auto` (\ref xy_auto) | For rounded corners, the radius of the curve along the Y axis. |
 */

/**
 * DOM object for the \ref xml_rect element.
 *
 * Use the `x`, `y`, `width` and `height` attributes to specify the rectangle's position and size,
 * and optionally round the corners with the `rx` and `ry` attributes.
 *
 *
 * ```xml
 * <rect x="10" y="50" width="280" height="200" fill="none" stroke="black" />
 * ```
 *
 * \htmlonly
 * <svg id="xml_rect" width="300" height="300" style="background-color: white">
 *   <style>
 *     #xml_rect text { font-size: 16px; font-weight: bold; color: black }
 *     #xml_rect rect { stroke-width: 2px }
 *   </style>
 *
 *   <rect x="10" y="50" width="280" height="200" fill="none" stroke="black" />
 *   <circle cx="10" cy="50" r="3" fill="black" />
 *   <text x="20" y="70">x,y</text>
 *   <text x="140" y="40">width</text>
 *   <text x="20" y="150">height</text>
 * </svg>
 * \endhtmlonly
 *
 * ```xml
 * <rect x="10" y="50" width="280" height="200" rx="50" ry="40" fill="none" stroke="black" />
 * ```
 *
 * \htmlonly
 * <svg id="xml_rect_r" width="300" height="300" style="background-color: white">
 *   <style>
 *     #xml_rect_r text { font-size: 16px; font-weight: bold; color: black }
 *     #xml_rect_r line { stroke: black; stroke-width: 2px; stroke-dasharray: 6,4 }
 *     #xml_rect_r rect { stroke-width: 2px }
 *   </style>
 *
 *   <rect x="10" y="50" width="280" height="200" rx="100" ry="100" fill="none" stroke="black" />
 *   <line x1="10" y1="150" x2="110" y2="150" />
 *   <text x="50" y="170">rx</text>
 *   <line x1="110" y1="150" x2="110" y2="50" />
 *   <text x="120" y="100">ry</text>
 * </svg>
 * \endhtmlonly
 */
class SVGRectElement : public SVGGeometryElement {
  friend class parser::SVGParserImpl;

private:
  /// Create an SVGRectElement wrapper from an entity.
  explicit SVGRectElement(EntityHandle handle) : SVGGeometryElement(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGRectElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::Rect;
  /// XML tag name, \ref xml_rect.
  static constexpr std::string_view Tag{"rect"};

  static_assert(SVGGeometryElement::IsBaseOf(Type));
  static_assert(SVGGraphicsElement::IsBaseOf(Type));

  /**
   * Create a new \ref xml_rect element.
   *
   * @param document Containing document.
   */
  static SVGRectElement Create(SVGDocument& document) {
    return CreateOn(CreateEmptyEntity(document));
  }

  /**
   * Set the top-left X coordinate.
   *
   * @param value Coordinate value.
   */
  void setX(Lengthd value);

  /**
   * Set the top-left Y coordinate.
   *
   * @param value Coordinate value.
   */
  void setY(Lengthd value);

  /**
   * Set the width dimension.
   *
   * @param value Dimension value.
   */
  void setWidth(Lengthd value);

  /**
   * Set the height dimension.
   *
   * @param value Dimension value.
   */
  void setHeight(Lengthd value);

  /**
   * Set the horizontal radius, along the X axis, for rounded corners.
   *
   * @param value Corner radius value, or `std::nullopt` for `auto`, to use the same value as `ry`
   * (see \ref xy_auto).
   */
  void setRx(std::optional<Lengthd> value);

  /**
   * Set the vertical radius, along the Y axis, for rounded corners.
   *
   * @param value Corner radius value, or `std::nullopt` for `auto`, to use the same value as `rx`
   * (see \ref xy_auto).
   */
  void setRy(std::optional<Lengthd> value);

  /**
   * Get the top-left X coordinate.
   *
   * @return Coordinate value.
   */
  Lengthd x() const;

  /**
   * Get the top-left Y coordinate.
   *
   * @return Coordinate value.
   */
  Lengthd y() const;

  /**
   * Get the width dimension.
   *
   * @return Dimension value.
   */
  Lengthd width() const;

  /**
   * Get the height dimension.
   *
   * @return Dimension value.
   */
  Lengthd height() const;

  /**
   * Get the horizontal radius, along the X axis, for rounded corners.
   *
   * @return Corner radius value, or `std::nullopt` for `auto`. To get the computed value, use \ref
   * computedRx().
   */
  std::optional<Lengthd> rx() const;

  /**
   * Get the vertical radius, along the Y axis, for rounded corners.
   *
   * @return Corner radius value, or `std::nullopt` for `auto`. To get the computed value, use \ref
   * computedRy().
   */
  std::optional<Lengthd> ry() const;

  /**
   * Get the computed top-left X coordinate.
   *
   * @return Coordinate value.
   */
  Lengthd computedX() const;

  /**
   * Get the computed top-left Y coordinate.
   *
   * @return Coordinate value.
   */
  Lengthd computedY() const;

  /**
   * Get the computed width dimension.
   *
   * @return Dimension value.
   */
  Lengthd computedWidth() const;

  /**
   * Get the computed height dimension.
   *
   * @return Dimension value.
   */
  Lengthd computedHeight() const;

  /**
   * Get the computed horizontal radius, along the X axis, for rounded corners.
   *
   * @return Corner radius value.
   */
  Lengthd computedRx() const;

  /**
   * Get the computed vertical radius, along the Y axis, for rounded corners.
   *
   * @return Corner radius value.
   */
  Lengthd computedRy() const;

  /**
   * Get the computed path of this rectangle, including rounded corners (if any).
   *
   * @return Computed path, or `std::nullopt` if the element is invalid (e.g. if the width or height
   * are zero).
   */
  std::optional<PathSpline> computedSpline() const;

private:
  void invalidate() const;
  void compute() const;
};

}  // namespace donner::svg
