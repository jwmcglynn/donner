#pragma once
/// @file

namespace donner::svg::components {

/**
 * Stores the marker data for an SVG element.
 */
struct MarkerComponent {
  double markerWidth;   //!< Width of the marker viewport.
  double markerHeight;  //!< Height of the marker viewport.
  double refX;          //!< X coordinate for the reference point of the marker.
  double refY;          //!< Y coordinate for the reference point of the marker.
  std::string orient;   //!< Orientation of the marker relative to the path.
};

}  // namespace donner::svg::components
