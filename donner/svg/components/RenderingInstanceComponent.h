#pragma once
/// @file

#include <optional>

#include "donner/base/Box.h"
#include "donner/base/Transform.h"
#include "donner/svg/components/filter/FilterEffect.h"
#include "donner/svg/core/ClipPathUnits.h"
#include "donner/svg/core/MarkerUnits.h"
#include "donner/svg/core/MaskUnits.h"
#include "donner/svg/properties/PaintServer.h"
#include "donner/svg/registry/Registry.h"

namespace donner::svg::components {

/**
 * Created on the start of a subtree, to inform the renderer about which element ends the current
 * subtree, plus how many isolated layers need to be popped when the subtree is complete.
 */
struct SubtreeInfo {
  /// Indicates the last entity within the current subtree. The renderer will continue rendering
  /// entities until it reaches this one, then it will pop \ref restorePopDepth isolated layers from
  /// the render state.
  Entity lastRenderedEntity;
  /// How many isolated layers to pop after rendering this entity.
  int restorePopDepth = 0;
};

/**
 * Contains rendering information for a paint server, such as the subtree needed if it establishes
 * an isolated layer, and where the paint server is located.
 */
struct PaintResolvedReference {
  /// Reference to the paint server element.
  ResolvedReference reference;
  /// Fallback color to use if this paint fails to instantiate. This can happen for various reasons,
  /// such as if a gradient has no stops (making it invalid).
  std::optional<css::Color> fallback;
  /// If this paint server creates a subtree, such as for patterns, contains subtree info to inform
  /// the renderer how to render it.
  std::optional<SubtreeInfo> subtreeInfo;
};

/**
 * Contains resolved information about the `clip-path` property, such as which element it is
 * pointing to.
 */
struct ResolvedClipPath {
  /// Reference to a \ref xml_clipPath element.
  ResolvedReference reference;
  /// The clip path units to use for this clip path.
  ClipPathUnits units;

  /// Returns true if the reference is valid, or false if this the \ref xml_clipPath did not
  /// properly resolve.
  bool valid() const { return reference.valid(); }
};

/**
 * Contains resolved information about the `mask` property, such as which element it is
 * pointing to.
 */
struct ResolvedMask {
  /// Reference to a \ref xml_mask element.
  ResolvedReference reference;
  /// Contains subtree info to inform the renderer how to render the mask.
  std::optional<SubtreeInfo> subtreeInfo;
  /// The mask content units to use for this mask.
  MaskContentUnits contentUnits;

  /// Returns true if the reference is valid, or false if this the \ref xml_mask did not
  /// properly resolve.
  bool valid() const { return reference.valid(); }
};

struct ResolvedMarker {
  /// Reference to a \ref xml_marker element.
  ResolvedReference reference;
  /// Contains subtree info to inform the renderer how to render the marker.
  std::optional<SubtreeInfo> subtreeInfo;
  /// Defines the coordinate system for marker attributes and contents.
  MarkerUnits markerUnits;

  /// Returns true if the reference is valid, or false if this the \ref xml_mask did not
  /// properly resolve.
  bool valid() const { return reference.valid(); }
};

/// The resolved paint server for a fill or stroke.
using ResolvedPaintServer =
    std::variant<PaintServer::None, PaintServer::Solid, PaintResolvedReference>;

/// The resolved filter effect for a filter.
using ResolvedFilterEffect = std::variant<std::vector<FilterEffect>, ResolvedReference>;

/**
 * Returns true if the paint server is not a \ref PaintServer::None.
 *
 * @param paint The paint server to check.
 * @return True if the paint server is not a \ref PaintServer::None.
 */
inline bool HasPaint(const ResolvedPaintServer& paint) {
  return !std::holds_alternative<PaintServer::None>(paint);
}

/**
 * An instance of the entity in the rendering tree. Each renderered entity has an instance with a
 * unique \c drawOrder, which enables this list to be sorted and traversed to render the tree.
 */
struct RenderingInstanceComponent {
  /// Default constructor.
  RenderingInstanceComponent() = default;

  /// The draw order of the element, computed from the traversal order of the tree.
  int drawOrder = 0;

  /// Whether the element is visible. Note that elements may still influence rendering behavior when
  /// they are hidden, such as \ref xml_pattern elements.
  bool visible = true;

  /// True if this instance establishes a new rendering layer, such as if there is opacity.
  bool isolatedLayer = false;

  /// The canvas-space transform of the element, element-from-world.
  Transformd entityFromWorldTransform;

  std::optional<Boxd> clipRect;  //!< The clip rect of the element, if set.

  std::optional<ResolvedClipPath> clipPath;  //!< The clip path of the element, if set.

  std::optional<ResolvedMask> mask;  //!< The mask of the element, if set.

  /// The resolved marker for marker-start, if any.
  std::optional<ResolvedMarker> markerStart;

  /// The resolved marker for marker-mid, if any.
  std::optional<ResolvedMarker> markerMid;

  /// The resolved marker for marker-end, if any.
  std::optional<ResolvedMarker> markerEnd;

  /**
   * The entity containing the structural components of the instance, element-specific
   * components like \ref IdComponent.
   */
  Entity dataEntity = entt::null;

  /**
   * The resolved paint server for the instance's fill, if any.
   */
  ResolvedPaintServer resolvedFill = PaintServer::None();

  /**
   * The resolved paint server for the instance's stroke, if any.
   */
  ResolvedPaintServer resolvedStroke = PaintServer::None();

  /**
   * The resolved filter effect on this instance, if any.
   */
  std::optional<ResolvedFilterEffect> resolvedFilter;

  /**
   * Information about this elements subtree, if there is a rendering-influencing subtree attached
   * to this entity.
   */
  std::optional<SubtreeInfo> subtreeInfo;

  /**
   * Shortcut for creating a handle for the \ref dataEntity, the entity containing the structural
   * components of the instance like \ref ClassComponent.
   *
   * @param registry The registry to use.
   * @return A handle for the \ref dataEntity.
   */
  EntityHandle dataHandle(Registry& registry) const { return EntityHandle(registry, dataEntity); }

  /**
   * A handle for the entity containing style information, which may be different than the \ref
   * dataHandle if this instance is within a shadow tree.
   *
   * @param registry The registry to use.
   * @return A handle for the style entity.
   */
  EntityHandle styleHandle(Registry& registry) const {
    return EntityHandle(registry,
                        entt::to_entity(registry.storage<RenderingInstanceComponent>(), *this));
  }

  /**
   * Return true if this is a shadow tree instance.
   *
   * @param registry The registry to use.
   * @return True if this is a shadow tree instance.
   */
  bool isShadow(Registry& registry) const {
    return entt::to_entity(registry.storage<RenderingInstanceComponent>(), *this) != dataEntity;
  }
};

}  // namespace donner::svg::components
