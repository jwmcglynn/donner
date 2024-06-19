#pragma once
/// @file

#include <vector>

#include "donner/svg/SVGGeometryElement.h"

namespace donner::svg {

/**
 * @page xml_polygon '<polygon>'
 * @ingroup elements_basic_shapes
 *
 * Creates a closed polygon with straight lines between the points, using the `points` attribute.
 *
 * - DOM object: SVGPolygonElement
 * - SVG2 spec: https://www.w3.org/TR/SVG2/shapes.html#PolygonElement
 *
 * ```xml
 * <polygon points="50,50 250,50 150,150 250,250 50,250" />
 * ```
 *
 * \htmlonly
 * <svg id="xml_polygon" width="300" height="300" style="background-color: white">
 *   <style>
 *     #xml_polygon polygon {
 *       fill: none;
 *       stroke: black;
 *       stroke-width: 2px;
 *       marker-mid: url(#arrow);
 *     }
 *     #xml_polygon circle { fill: black; r: 3px }
 *     #xml_polygon text { font-size: 16px; font-weight: bold; color: black }
 *   </style>
 *   <polygon points="50,50 250,50 150,150 250,250 50,250" />
 *   <circle cx="50" cy="50" style="fill: red" />
 *   <text x="30" y="43">50,50</text>
 *   <path d="M0,0 L10,5 L0,10 Z" transform="translate(150,45)" fill="red" />
 *   <circle cx="250" cy="50" />
 *   <text x="230" y="43">250,50</text>
 *   <circle cx="150" cy="150" />
 *   <text x="160" y="155">150,150</text>
 *   <circle cx="250" cy="250" />
 *   <text x="230" y="268">250,250</text>
 *   <circle cx="50" cy="250" />
 *   <text x="30" y="268">50,250</text>
 * </svg>
 * \endhtmlonly
 *
 * | Attribute | Default | Description  |
 * | --------: | :-----: | :----------- |
 * | `points`  | (none)  | List of points that make up the polygon, see \ref poly_points. |
 */

/**
 * DOM object for a \ref xml_polygon element.
 *
 * The `points` attribute defines a closed polygon with straight lines between the points.
 *
 * \htmlonly
 * <svg id="xml_polygon" width="300" height="300" style="background-color: white">
 *   <style>
 *     #xml_polygon polygon {
 *       fill: none;
 *       stroke: black;
 *       stroke-width: 2px;
 *       marker-mid: url(#arrow);
 *     }
 *     #xml_polygon circle { fill: black; r: 3px }
 *     #xml_polygon text { font-size: 16px; font-weight: bold; color: black }
 *   </style>
 *   <polygon points="50,50 250,50 150,150 250,250 50,250" />
 *   <circle cx="50" cy="50" style="fill: red" />
 *   <text x="30" y="43">50,50</text>
 *   <path d="M0,0 L10,5 L0,10 Z" transform="translate(150,45)" fill="red" />
 *   <circle cx="250" cy="50" />
 *   <text x="230" y="43">250,50</text>
 *   <circle cx="150" cy="150" />
 *   <text x="160" y="155">150,150</text>
 *   <circle cx="250" cy="250" />
 *   <text x="230" y="268">250,250</text>
 *   <circle cx="50" cy="250" />
 *   <text x="30" y="268">50,250</text>
 * </svg>
 * \endhtmlonly
 */
class SVGPolygonElement : public SVGGeometryElement {
private:
  /// Create an SVGPolygonElement wrapper from an entity.
  explicit SVGPolygonElement(EntityHandle handle) : SVGGeometryElement(handle) {}

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::Polygon;
  /// XML tag name, \ref xml_polygon.
  static constexpr std::string_view Tag{"polygon"};

  /**
   * Create a new \ref xml_polygon element.
   *
   * @param document Containing document.
   */
  static SVGPolygonElement Create(SVGDocument& document);

  /**
   * Set the polygon's points, which will be used to draw a closed polygon with straight lines
   * between them.
   *
   * @param value Points list.
   */
  void setPoints(std::vector<Vector2d> value);

  /**
   * Get the polygon's points.
   */
  const std::vector<Vector2d>& points() const;

private:
  void invalidate() const;
};

}  // namespace donner::svg
