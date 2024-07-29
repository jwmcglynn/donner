#pragma once
/// @file

#include <optional>

#include "donner/base/Box.h"
#include "donner/base/Transform.h"
#include "donner/svg/components/filter/FilterEffect.h"
#include "donner/svg/core/ClipPathUnits.h"
#include "donner/svg/properties/PaintServer.h"
#include "donner/svg/registry/Registry.h"

namespace donner::svg::components {

struct SubtreeInfo {
  Entity lastRenderedEntity;
  int restorePopDepth = 0;  //!< How many isolated layers to pop after rendering this entity.
};

struct PaintResolvedReference {
  ResolvedReference reference;
  std::optional<css::Color> fallback;
  std::optional<SubtreeInfo> subtreeInfo;
};

struct ResolvedClipPath {
  ResolvedReference reference;
  ClipPathUnits units;

  bool valid() const { return reference.valid(); }
};

using ResolvedPaintServer =
    std::variant<PaintServer::None, PaintServer::Solid, PaintResolvedReference>;

using ResolvedFilterEffect = std::variant<std::vector<FilterEffect>, ResolvedReference>;

inline bool HasPaint(const ResolvedPaintServer& paint) {
  return !std::holds_alternative<PaintServer::None>(paint);
}

struct RenderingInstanceComponent {
  RenderingInstanceComponent() = default;

  int drawOrder =
      0;  //!< The draw order of the element, computed from the traversal order of the tree.
  bool visible = true;  //!< Whether the element is visible. Note that elements may still influence
                        //!< rendering behavior when they are hidden, such as \ref pattern elements.
  bool isolatedLayer = false;  //!< True if this instance establishes a new rendering layer, such as
                               //!< if there is opacity.

  Transformd entityFromWorldTransform;  //!< The canvas-space transform of the element,
                                        //!< element-from-world.

  std::optional<Boxd> clipRect;  //!< The clip rect of the element, if set.

  std::optional<ResolvedClipPath> clipPath;  //!< The clip path of the element, if set.

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
