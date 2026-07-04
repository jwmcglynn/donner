#pragma once
/// @file

#include "donner/base/Length.h"
#include "donner/svg/core/MarkerOrient.h"
#include "donner/svg/core/MarkerUnits.h"

namespace donner::svg::components {

/**
 * Stores the marker data for an SVG element.
 *
 * Lengths are stored unresolved (\ref Lengthd) so that percentage units - which resolve against the
 * viewport of the element referencing the marker - are computed at render time.
 * `markerWidth`/`refX` resolve against the viewport width (\ref Lengthd::Extent::X) and
 * `markerHeight`/`refY` against the viewport height (\ref Lengthd::Extent::Y).
 */
struct MarkerComponent {
  Lengthd markerWidth{3.0, Lengthd::Unit::None};   //!< Width of the marker viewport.
  Lengthd markerHeight{3.0, Lengthd::Unit::None};  //!< Height of the marker viewport.
  Lengthd refX{0.0, Lengthd::Unit::None};          //!< X coordinate for the reference point.
  Lengthd refY{0.0, Lengthd::Unit::None};          //!< Y coordinate for the reference point.

  MarkerOrient orient;  //!< Orientation of the marker.

  MarkerUnits markerUnits =
      MarkerUnits::Default;  //!< Coordinate system for marker attributes and contents.
};

}  // namespace donner::svg::components
