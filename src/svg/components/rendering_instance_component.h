#pragma once

#include <optional>

#include "src/base/box.h"
#include "src/base/transform.h"
#include "src/svg/properties/paint_server.h"
#include "src/svg/registry/registry.h"

namespace donner::svg {

struct PaintResolvedReference {
  ResolvedReference reference;
  std::optional<css::Color> fallback;
};

using ResolvedPaintServer =
    std::variant<PaintServer::None, PaintServer::Solid, PaintResolvedReference>;

inline bool HasPaint(const ResolvedPaintServer& paint) {
  return !std::holds_alternative<PaintServer::None>(paint);
}

struct RenderingInstanceComponent {
  RenderingInstanceComponent() = default;

  int drawOrder =
      0;  //!< The draw order of the element, computed from the traversal order of the tree.
  bool visible = true;  //!< Whether the element is visible. Note that elements may still influence
                        //!< rendering behavior when they are hidden, such as <pattern> elements.
  bool isolatedLayer = false;  //!< True if this instance establishes a new rendering layer, such as
                               //!< if there is opacity.
  int restorePopDepth = 0;     //!< How many isolated layers to pop before rendering this instance.

  Transformd
      transformCanvasSpace;  //!< The canvas-space transform of the element, element-from-canvas.

  std::optional<Boxd> clipRect;  //!< The clip rect of the element, if set.

  /**
   * The entity containing the structural components of the instance, element-specific
   * components like \ref IdComponent.
   */
  Entity dataEntity;

  /**
   * The resolved paint server for the instance's fill, if any.
   */
  ResolvedPaintServer resolvedFill = PaintServer::None();

  /**
   * The resolved paint server for the instance's stroke, if any.
   */
  ResolvedPaintServer resolvedStroke = PaintServer::None();

  /**
   * Shortcut for creating a handle for the \ref dataEntity, the entity containing the structural
   * components of the instance like \ref ClassComponent.
   *
   * @param registry The registry to use.
   * @return A handle for the \ref dataEntity.
   */
  EntityHandle dataHandle(Registry& registry) { return EntityHandle(registry, dataEntity); }

  /**
   * A handle for the entity containing style information, which may be different than the \ref
   * dataHandle if this instance is within a shadow tree.
   *
   * @param registry The registry to use.
   * @return A handle for the style entity.
   */
  EntityHandle styleHandle(Registry& registry) {
    return EntityHandle(registry, entt::to_entity(registry, *this));
  }

  /**
   * Return true if this is a shadow tree instance.
   *
   * @param registry The registry to use.
   * @return True if this is a shadow tree instance.
   */
  bool isShadow(Registry& registry) const { return entt::to_entity(registry, *this) == dataEntity; }
};

}  // namespace donner::svg
