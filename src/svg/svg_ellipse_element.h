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
protected:
  explicit SVGEllipseElement(EntityHandle handle) : SVGGeometryElement(handle) {}

public:
  static constexpr ElementType Type = ElementType::Ellipse;
  static constexpr std::string_view Tag = "ellipse";

  static SVGEllipseElement Create(SVGDocument& document);

  void setCx(Lengthd value);
  void setCy(Lengthd value);
  void setRx(std::optional<Lengthd> value);
  void setRy(std::optional<Lengthd> value);

  Lengthd cx() const;
  Lengthd cy() const;
  std::optional<Lengthd> rx() const;
  std::optional<Lengthd> ry() const;

  Lengthd computedCx() const;
  Lengthd computedCy() const;
  Lengthd computedRx() const;
  Lengthd computedRy() const;

private:
  void invalidate() const;
  void compute() const;
};

}  // namespace donner::svg
