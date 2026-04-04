#pragma once
/// @file

#include <any>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/EcsRegistry.h"
#include "donner/base/ParseError.h"
#include "donner/base/Vector2.h"

namespace donner::svg::components {

/**
 * Rendering controller, which instantiates and and manages the rendering tree.
 *
 * Used during the rendering phase in combination with the rendering backend.
 */
class RenderingContext {
public:
  /// Constructor.
  explicit RenderingContext(Registry& registry);

  /**
   * Create the render tree for the document, optionally returning parse warnings found when parsing
   * deferred parts of the tree.
   *
   * @param verbose If true, enable verbose logging.
   * @param outWarnings If non-null, warnings will be added to this vector.
   */
  void instantiateRenderTree(bool verbose, std::vector<ParseError>* outWarnings);

  /**
   * Create all computed components needed for text/layout/style queries without instantiating
   * render instances.
   *
   * @param outWarnings If non-null, warnings will be added to this vector.
   */
  void ensureComputedComponents(std::vector<ParseError>* outWarnings);

  /**
   * Find the first entity that intersects the given point, using spatial acceleration
   * when the document has enough elements.
   *
   * @param point Point to find the intersecting entity for
   */
  Entity findIntersecting(const Vector2d& point);

  /**
   * Find all entities that intersect the given point, ordered front-to-back.
   *
   * @param point Point to find intersecting entities for
   */
  std::vector<Entity> findAllIntersecting(const Vector2d& point);

  /**
   * Find all entities whose world-space bounds intersect the given rectangle,
   * ordered front-to-back. Uses the spatial grid for acceleration when available.
   *
   * @param rect Rectangle in world coordinates to test intersection against
   */
  std::vector<Entity> findIntersectingRect(const Boxd& rect);

  /**
   * Get the world-space bounding box of an entity (fill bounds only, not including stroke).
   *
   * @param entity Entity to get bounds for
   * @return The world-space AABB, or std::nullopt if the entity has no shape
   */
  std::optional<Boxd> getWorldBounds(Entity entity);

  /**
   * Invalidate the rendering tree, forcing it to be recreated on the next render.
   */
  void invalidateRenderTree();

  /**
   * Set initial context-fill and context-stroke values for sub-document rendering.
   * When a `<use>` element references an external SVG, the `<use>` element's resolved fill
   * and stroke become the initial context-fill/context-stroke in the sub-document.
   *
   * Values are type-erased `ResolvedPaintServer` stored as `std::any` to avoid header
   * dependencies on component types.
   *
   * @param fill Initial context-fill value (std::any containing ResolvedPaintServer).
   * @param stroke Initial context-stroke value (std::any containing ResolvedPaintServer).
   */
  void setInitialContextPaint(std::any fill, std::any stroke);

  /**
   * Instantiate a render tree for a single element subtree, ignoring \c Nonrenderable behavior.
   * Used for feImage fragment references that need to render elements inside `<defs>`.
   *
   * @param targetEntity The entity to render (root of the subtree).
   * @param verbose If true, enable verbose logging.
   * @return The last entity in the instantiated subtree (for use with traverseRange).
   */
  Entity instantiateSubtreeForStandaloneRender(Entity targetEntity, bool verbose);

private:
  /**
   * Test whether a single entity is hit by the given point, applying pointer-events rules.
   *
   * @param entity Entity to test
   * @param point Point in world coordinates
   * @return true if the entity is hit
   */
  bool hitTestEntity(Entity entity, const Vector2d& point);

  /**
   * Create all computed parts of the tree, evaluating styles and creating shadow trees.
   *
   * @param outWarnings If non-null, warnings will be added to this vector.
   */
  void createComputedComponents(std::vector<ParseError>* outWarnings);

  /**
   * Creates all rendering instances for the document, the final step before it can be rendered.
   *
   * @param verbose If true, enable verbose logging.
   */
  void instantiateRenderTreeWithPrecomputedTree(bool verbose);

private:
  /// Reference to the registry containing the render tree.
  Registry& registry_;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)

  /// Initial context-fill value for sub-document rendering (type-erased ResolvedPaintServer).
  std::any initialContextFill_;
  /// Initial context-stroke value for sub-document rendering (type-erased ResolvedPaintServer).
  std::any initialContextStroke_;
};

}  // namespace donner::svg::components
