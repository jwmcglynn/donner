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

  /// Lazily-populated cache for `localBounds()`. Left as a public data
  /// member (rather than hidden behind a `private:` section) so the
  /// component stays an aggregate — entt's `emplace_or_replace<T>(args...)`
  /// path initializes components via aggregate initialization on older
  /// compilers and breaks once a non-public section is added.
  mutable std::optional<Box2d> cachedLocalBounds;

  /**
   * Returns the tight fill bounds of the path in local (pre-transform) space.
   *
   * Memoized — `Path::bounds()` walks every command (O(N) in path size), so
   * hot-path callers (culling, hit-testing, filter-region computation) should
   * prefer this accessor. The cache is tied to the `ComputedPathComponent`'s
   * lifetime; `ShapeSystem` rebuilds the component whenever the underlying
   * geometry changes, which invalidates the cache. Style-only changes
   * (fill color, opacity, stroke-width) leave the component — and the
   * cached bounds — intact.
   */
  Box2d localBounds() const {
    if (!cachedLocalBounds) {
      cachedLocalBounds = spline.bounds();
    }
    return *cachedLocalBounds;
  }

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
