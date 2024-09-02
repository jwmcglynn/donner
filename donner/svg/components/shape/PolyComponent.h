#pragma once
/// @file

#include <vector>

#include "donner/base/Vector2.h"

namespace donner::svg::components {

/**
 * Parameters for a \ref xml_polygon or \ref xml_polyline element.
 *
 * The difference between a \ref xml_polygon and a \ref xml_polyline is that a polygon is a closed
 * shape, and draws the closing line segment to connect the first and last points. A polyline is a
 * open shape, and does not draw the closing line segment.
 */
struct PolyComponent {
  /// Polygon type, either a closed polygon or a polyline (list of line segments)
  enum class Type {
    Polygon,  //!< \ref xml_polygon closed shape
    Polyline  //!< \ref xml_polyline list of line segments
  };

  /**
   * Constructor, creates a polygon or polyline.
   *
   * @param type The type of polygon to create.
   */
  explicit PolyComponent(Type type) : type(type) {}

  /// The type of polygon, either a closed polygon or a polyline (list of line segments)
  Type type;

  /// The points of the polygon.
  std::vector<Vector2d> points;
};

}  // namespace donner::svg::components
