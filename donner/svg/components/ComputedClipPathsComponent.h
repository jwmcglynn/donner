#pragma once
/// @file

#include "donner/base/Transform.h"
#include "donner/svg/core/ClipRule.h"
#include "donner/svg/core/PathSpline.h"

namespace donner::svg::components {

/**
 * Stores a computed clip path, which is used to clip the rendering of an entity.
 *
 * Aggregates together all paths that compose a clip path, including nested clip paths.
 */
struct ComputedClipPathsComponent {
  /// Information about a specific shape within a clip path.
  struct ClipPath {
    /// The path of the clip path.
    PathSpline path;

    /// Transform to the clip path from the parent entity.
    Transformd entityFromParent;

    /// Computed clip rule for this path.
    ClipRule clipRule = ClipRule::NonZero;

    /**
     * Layer index of the clip path, to create a new clip path the layer is incremented. Paths
     * within a layer are union'd together. When the layer decreases, the combined path of
     * everything in the layer is differenced with the next path in the list.
     */
    int layer = 0;
  };

  /// All clip paths, in order they need to be applied based on their layer.
  std::vector<ClipPath> clipPaths;
};

}  // namespace donner::svg::components
