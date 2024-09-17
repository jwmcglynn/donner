#pragma once
/// @file

#include <optional>

#include "donner/svg/components/PreserveAspectRatioComponent.h"
#include "donner/svg/core/MarkerUnits.h"

namespace donner::svg::components {

/**
 * Stores the marker data for an SVG element.
 */
struct MarkerComponent {
  double markerWidth;   //!< Width of the marker viewport.
  double markerHeight;  //!< Height of the marker viewport.
  double refX;          //!< X coordinate for the reference point of the marker.
  double refY;          //!< Y coordinate for the reference point of the marker.

  bool orientAuto;     //!< True if orient="auto", false if a fixed angle.
  double orientAngle;  //!< Angle value if orient is a numeric value.

  MarkerUnits markerUnits;  //!< Coordinate system for marker attributes and contents.

  std::optional<Boxd> viewBox;              //!< Optional viewBox attribute.
  PreserveAspectRatio preserveAspectRatio;  //!< preserveAspectRatio property.
};

}  // namespace donner::svg::components
