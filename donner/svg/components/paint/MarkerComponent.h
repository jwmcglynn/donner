#pragma once
/// @file

#include <optional>

#include "donner/svg/core/MarkerOrient.h"
#include "donner/svg/core/MarkerUnits.h"
#include "donner/svg/core/PreserveAspectRatio.h"

namespace donner::svg::components {

/**
 * Stores the marker data for an SVG element.
 */
struct MarkerComponent {
  double markerWidth = 3.0;   //!< Width of the marker viewport.
  double markerHeight = 3.0;  //!< Height of the marker viewport.
  double refX = 0.0;          //!< X coordinate for the reference point of the marker.
  double refY = 0.0;          //!< Y coordinate for the reference point of the marker.

  MarkerOrient orient;  //!< Orientation of the marker.

  MarkerUnits markerUnits =
      MarkerUnits::Default;  //!< Coordinate system for marker attributes and contents.
};

}  // namespace donner::svg::components
