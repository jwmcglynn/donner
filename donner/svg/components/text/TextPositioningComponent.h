#pragma once
/// @file

#include "donner/base/Length.h"
#include "donner/base/SmallVector.h"

namespace donner::svg::components {

/**
 * Defines the positioning of a text element, which may have other text elements as children.
 * Created on each \ref xml_text and \ref xml_tspan element.
 */
struct TextPositioningComponent {
  /// X coordinate for each character. If empty, the property is not set.
  SmallVector<Lengthd, 1> x;

  /// Y coordinate for each character. If empty, the property is not set.
  SmallVector<Lengthd, 1> y;

  /// Relative shift in x for each character. If empty, the property is not set.
  SmallVector<Lengthd, 1> dx;

  /// Relative shift in y for each character. If empty, the property is not set.
  SmallVector<Lengthd, 1> dy;

  /// Rotation in degrees for each character. If empty, the property is not set.
  SmallVector<double, 1> rotateDegrees;
};

}  // namespace donner::svg::components
