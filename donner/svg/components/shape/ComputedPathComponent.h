#pragma once
/// @file

#include <optional>

#include "donner/svg/core/PathSpline.h"

namespace donner::svg::components {

/**
 * Stores a PathSpline used for rendering a shape, which may be generated from the parameters of
 * shapes such as circle, rect, line, poly, and path.
 *
 * All shapes create computed paths, and these paths can be rendered using a unified rendering
 * pipeline.
 */
struct ComputedPathComponent {
  /// PathSpline used for rendering the shape.
  PathSpline spline;

  /// Cached box of the shape, if it was previously computed. If not, this can be calculated using
  /// the \ref PathSpline::bounds() method.
  std::optional<Boxd> cachedBounds;

  /// Returns the bounds of the shape. May calculate the bounds on-demand if they have not been
  /// previously computed.
  Boxd bounds() {
    if (!cachedBounds) {
      cachedBounds = spline.bounds();
    }

    return cachedBounds.value();
  }
};

}  // namespace donner::svg::components
