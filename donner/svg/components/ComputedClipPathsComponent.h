#pragma once
/// @file

#include "donner/base/Path.h"
#include "donner/base/Transform.h"
#include "donner/svg/core/ClipRule.h"
#include "donner/svg/resources/FontCatalogTypes.h"

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
    Path path;

    /// Transform to the clip path from the parent entity.
    Transform2d parentFromEntity;

    /// Computed clip rule for this path.
    ClipRule clipRule = ClipRule::NonZero;

    /// Basic shapes specify their own rule, independently of the element clip-rule property.
    bool hasOwnClipRule = false;

    /**
     * Nested clip paths immediately precede their referencing shape at a greater layer index.
     * Their union intersects only that shape. The resulting sibling shapes at the same layer
     * are unioned together.
     */
    int layer = 0;

    /// Nonzero for a group marker: number of immediately preceding records forming its geometry.
    std::size_t groupPathCount = 0;
  };

  /// All clip paths, in order they need to be applied based on their layer.
  std::vector<ClipPath> clipPaths;
  /// Faces whose accepted glyph/shape paths formed this consumer's clip, including nested clips.
  std::vector<FontFaceDependency> fontDependencies;
  uint64_t fontResourceRevision = 0;
};

}  // namespace donner::svg::components
