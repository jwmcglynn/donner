#pragma once
/// @file

#include <vector>

#include "donner/svg/SVGGeometryElement.h"

namespace donner::svg {

/**
 * @page xml_polyline '<polyline>'
 * @ingroup elements_basic_shapes
 *
 * Creates a set of connected straight line segments, using the `points` attribute.
 *
 * - DOM object: SVGPolylineElement
 * - SVG2 spec: https://www.w3.org/TR/SVG2/shapes.html#PolylineElement
 *
 * ```xml
 * <polyline points="50,50 250,50 150,150 250,250 50,250" />
 * ```
 *
 * \htmlonly
 * <svg id="xml_polyline" width="300" height="300" style="background-color: white">
 *   <style>
 *     #xml_polyline polyline {
 *       fill: none;
 *       stroke: black;
 *       stroke-width: 2px;
 *       marker-mid: url(#arrow);
 *     }
 *     #xml_polyline circle { fill: black; r: 3px }
 *     #xml_polyline text { font-size: 16px; font-weight: bold; color: black }
 *   </style>
 *   <polyline points="50,50 250,50 150,150 250,250 50,250" />
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
 * | `points`  | (none)  | List of points to connect with line segments, see \ref poly_points. |
 */

/**
 * DOM object for a \ref xml_polyline element.
 *
 * The `points` attribute defines a list of points to connect with line segments.
 *
 * \htmlonly
 * <svg id="xml_polyline" width="300" height="300" style="background-color: white">
 *   <style>
 *     #xml_polyline polyline {
 *       fill: none;
 *       stroke: black;
 *       stroke-width: 2px;
 *       marker-mid: url(#arrow);
 *     }
 *     #xml_polyline circle { fill: black; r: 3px }
 *     #xml_polyline text { font-size: 16px; font-weight: bold; color: black }
 *   </style>
 *   <polyline points="50,50 250,50 150,150 250,250 50,250" />
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
class SVGPolylineElement : public SVGGeometryElement {
protected:
  /// Create an SVGPolylineElement wrapper from an entity.
  explicit SVGPolylineElement(EntityHandle handle) : SVGGeometryElement(handle) {}

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::Polyline;
  /// XML tag name, \ref xml_polyline.
  static constexpr std::string_view Tag{"polyline"};

  static_assert(SVGGeometryElement::IsBaseOf(Type));

  /**
   * Create a new \ref xml_polyline element.
   *
   * @param document Containing document.
   */
  static SVGPolylineElement Create(SVGDocument& document);

  /**
   * Set the line points, which will be connected with straight line segments.
   *
   * @param value Points list.
   */
  void setPoints(std::vector<Vector2d> value);

  /**
   * Get the line points, which the polyline connects with straight line segments.
   *
   * @return Points list.
   */
  const std::vector<Vector2d>& points() const;

private:
  void invalidate() const;
};

}  // namespace donner::svg
