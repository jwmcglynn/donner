#pragma once
/// @file

#include <optional>

#include "donner/base/Path.h"
#include "donner/base/RcString.h"

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
  /// component stays an aggregate - entt's `emplace_or_replace<T>(args...)`
  /// path initializes components via aggregate initialization on older
  /// compilers and breaks once a non-public section is added.
  mutable std::optional<Box2d> cachedLocalBounds;

  /**
   * The path-data string that `spline` was parsed from, when the spline came from path data.
   *
   * `std::nullopt` for every other producer: shapes generated from numeric attributes
   * (circle, ellipse, line, poly, rect), splines supplied directly instead of parsed, and
   * path data whose parse reported a diagnostic.
   *
   * `ShapeSystem` uses this to skip re-parsing unchanged path data. Parsing is a pure function
   * of the resolved path-data string, so while the string is unchanged the retained `spline` is
   * byte-for-byte what a re-parse would produce, and the shape pass can return the retained
   * component untouched. Keeping the key on the computed component rather than beside the
   * source attribute ties its lifetime to the geometry it describes: every writer of this
   * component either sets the key to the string it just parsed or clears it, and every
   * invalidation removes or replaces the component, so a stale key cannot outlive the spline
   * it belongs to.
   *
   * Cost: `sizeof(std::optional<RcString>)` is 40 bytes, paid by every `ComputedPathComponent`,
   * including the numeric-attribute shapes that always leave it empty. Beyond that, path data
   * longer than \ref RcString's inline capacity shares its buffer with the attribute value it
   * was copied from and costs nothing more; shorter path data is held inline, so it is copied.
   */
  std::optional<RcString> sourcePathData;

  /**
   * Returns the tight fill bounds of the path in local (pre-transform) space.
   *
   * Memoized - `Path::bounds()` walks every command (O(N) in path size), so
   * hot-path callers (culling, hit-testing, filter-region computation) should
   * prefer this accessor. The cache is tied to the `ComputedPathComponent`'s
   * lifetime; `ShapeSystem` rebuilds the component whenever the underlying
   * geometry changes, which invalidates the cache. Style-only changes
   * (fill color, opacity, stroke-width) leave the component - and the
   * cached bounds - intact.
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
