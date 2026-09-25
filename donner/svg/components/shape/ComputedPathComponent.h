#pragma once
/// @file

#include <optional>

#include "donner/base/Path.h"
#include "donner/base/RcString.h"
#include "donner/base/Transform.h"

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
   * The transform that produced \ref cachedHostSpline, or `std::nullopt` when nothing is cached.
   *
   * Public and mutable for the same reason as `cachedLocalBounds`: the component has to stay
   * an aggregate.
   */
  mutable std::optional<Transform2d> cachedHostFromLocal;

  /**
   * Cache of \ref spline mapped through \ref cachedHostFromLocal. @see hostSpaceSpline.
   *
   * Retained for the component's lifetime once populated, like \ref cachedLocalBounds, and not
   * released when an element stops resolving to a host-space stroke. A style change that turns
   * `vector-effect` off therefore leaves one mapped path per shape behind until `ShapeSystem`
   * replaces the component or the entity is destroyed. That is bounded by the geometry the
   * document already holds (one extra copy of a spline that is itself retained), so it is not
   * metered by the Geode geometry budget, which meters the encode and stroke-outline products
   * built from it.
   */
  mutable Path cachedHostSpline;

  /// Memoized arc length of \ref spline. @see localPathLength.
  mutable std::optional<double> cachedLocalPathLength;

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

  /**
   * Returns \ref spline mapped into host (root canvas) space by \p hostFromLocal.
   *
   * `vector-effect: non-scaling-stroke` under a non-similarity CTM is stroked from this geometry
   * on every frame, so the mapped path is memoized here rather than rebuilt per draw. Both inputs
   * are part of the key: the component's lifetime covers the spline (`ShapeSystem` replaces the
   * component whenever the geometry changes, exactly as for \ref cachedLocalBounds), and the
   * stored transform covers the CTM, which a transform-only DOM mutation, a canvas resize, or a
   * second `<use>` instance of the same shape changes without touching the spline.
   *
   * One slot, so the memo pays off for one host-space instance of a shape per frame. Two `<use>`
   * copies of one shape under different CTMs in the same frame are each correct, but they evict
   * one another and rebuild the mapped path on every draw; the backends' own per-entity slots
   * behave the same way. Multi-way caching is deliberately not implemented here.
   *
   * The returned reference is invalidated by the next call with a different transform.
   *
   * @param hostFromLocal Transform from the element's local space to host space.
   */
  const Path& hostSpaceSpline(const Transform2d& hostFromLocal) const {
    if (!cachedHostFromLocal.has_value() || *cachedHostFromLocal != hostFromLocal) {
      cachedHostSpline = spline.transformed(hostFromLocal);
      cachedHostFromLocal = hostFromLocal;
    }

    return cachedHostSpline;
  }

  /**
   * Returns the arc length of \ref spline in local (pre-transform) space.
   *
   * Memoized - `Path::pathLength()` subdivides every curve, and a dashed `pathLength` stroke asks
   * for it on every draw. A pure function of the spline, so unlike \ref hostSpaceSpline it needs
   * no key: the component's lifetime is the whole invalidation story, exactly as for
   * \ref cachedLocalBounds.
   */
  double localPathLength() const {
    if (!cachedLocalPathLength.has_value()) {
      cachedLocalPathLength = spline.pathLength();
    }

    return *cachedLocalPathLength;
  }
};

}  // namespace donner::svg::components
