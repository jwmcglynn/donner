#pragma once
/// @file

#include <optional>

#include "donner/base/Path.h"

namespace donner::svg::components {

/**
 * Stores a Path used for rendering a shape, which may be generated from the parameters of
 * shapes such as circle, rect, line, poly, and path.
 *
 * All shapes create computed paths, and these paths can be rendered using a unified rendering
 * pipeline.
 */
struct ComputedPathComponent {
  /// Path used for rendering the shape.
  Path spline;

  /**
   * Returns the tight bounds of the shape, transformed to the target coordinate system.
   *
   * @param pathFromTarget Transform to transform the path to the target coordinate system.
   */
  Box2d transformedBounds(const Transform2d& pathFromTarget) {
    return spline.transformedBounds(pathFromTarget);
  }
};

}  // namespace donner::svg::components
