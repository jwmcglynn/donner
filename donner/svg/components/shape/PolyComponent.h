#pragma once
/// @file

#include <vector>

#include "donner/base/Vector2.h"

namespace donner::svg::components {

/**
 * Parameters for a \ref xml_polygon or \ref xml_polyline element.
 */
struct PolyComponent {
  /// Polygon type, either a closed polygon or a polyline (list of line segments)
  enum class Type {
    Polygon,  //!< \ref xml_polygon closed shape
    Polyline  //!< \ref xml_polyline list of line segments
  };

  explicit PolyComponent(Type type) : type(type) {}

  Type type;
  std::vector<Vector2d> points;
};

}  // namespace donner::svg::components
