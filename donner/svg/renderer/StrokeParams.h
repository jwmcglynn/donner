#pragma once
/// @file

#include <vector>

#include "donner/svg/core/Stroke.h"

namespace donner::svg {

/**
 * Stroke configuration used for path and primitive drawing.
 */
struct StrokeParams {
  /// Stroke width in user units.
  double strokeWidth = 0.0;
  StrokeLinecap lineCap = StrokeLinecap::Butt;
  StrokeLinejoin lineJoin = StrokeLinejoin::Miter;
  /// Maximum miter ratio before converting to bevel.
  double miterLimit = 4.0;
  /// Dash pattern lengths alternating on/off segments.
  std::vector<double> dashArray;
  /// Dash phase offset.
  double dashOffset = 0.0;
  /// SVG pathLength attribute value; 0 means unused. When non-zero, dash arrays and offsets are
  /// scaled by the ratio of the actual path length to this value.
  double pathLength = 0.0;
};

}  // namespace donner::svg
