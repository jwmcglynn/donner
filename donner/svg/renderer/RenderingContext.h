#pragma once
/// @file

#include <optional>
#include <utility>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/EcsRegistry.h"
#include "donner/base/ParseDiagnostic.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/base/Path.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/core/HitTestMode.h"
#include "donner/svg/renderer/AnimationPresentation.h"

namespace donner::svg::components {

/**
 * Rendering controller, which instantiates and and manages the rendering tree.
 *
 * Used during the rendering phase in combination with the rendering backend.
 */
class RenderingContext {
public:
  /// Evaluated pointer-event geometry shared by live queries and immutable frame captures.
  struct HitGeometry {
    Box2d documentBounds;  ///< Shape bounds in document space, inflated by the stroke width.
    std::optional<Box2d> textBounds;  ///< Text ink bounds in entity space; set for text roots.
    Transform2d entityFromDocumentTransform;  ///< Maps document-space points to entity space.
    FillRule fillRule = FillRule::NonZero;    ///< Computed fill rule for the fill test.
    double strokeWidth = 0.0;  ///< Computed stroke width, or zero when the stroke is unpainted.
    bool testFill = false;     ///< Whether the fill area is hit-testable.
    bool testStroke = false;   ///< Whether the stroke area is hit-testable.
    bool boundsOnly = false;   ///< Whether `pointer-events: bounding-box` hits the whole bounds.

    /// Test a point using an optional prepared path borrowed for the duration of this call.
    /// @param point Document-space point. @param path Prepared shape path, or null for bounds.
    bool contains(const Vector2d& point, const Path* path) const;
  };
  /// Owned geometry and internal target identity in front-to-back paint order.
  struct HitRecord {
    Entity entity = entt::null;
    HitGeometry geometry;
    Path path;
  };
  /// Capture bounded hit geometry; nullopt rejects the whole snapshot on budget exhaustion.
  /// @param maximumRecords Lowerable instance budget, capped at 65536.
  /// @param maximumPathBytes Lowerable path-storage budget, capped at 32 MiB.
  std::optional<std::vector<HitRecord>> captureHitGeometry(
      std::size_t maximumRecords = 65536, std::size_t maximumPathBytes = 32 * 1024 * 1024);

  /// Constructor.
  explicit RenderingContext(Registry& registry);

  /**
   * Create the render tree for the document, optionally returning parse warnings found when parsing
   * deferred parts of the tree.
   *
   * @param verbose If true, enable verbose logging.
   * @param warningSink Sink to collect warnings.
   */
  void instantiateRenderTree(bool verbose, ParseWarningSink& warningSink);

  /**
   * Create all computed components needed for text/layout/style queries without instantiating
   * render instances.
   *
   * @param warningSink Sink to collect warnings.
   */
  void ensureComputedComponents(ParseWarningSink& warningSink);

  /**
   * Find the first entity that intersects the given point, using spatial acceleration
   * when the document has enough elements. Offscreen content is never hit; a hit on `<use>`
   * content returns its shadow entity, see \ref EventTargetEntity.
   *
   * @param point Point to find the intersecting entity for
   * @param mode Hit eligibility policy.
   */
  Entity findIntersecting(const Vector2d& point, HitTestMode mode = HitTestMode::SvgPointerEvents);

  /**
   * Find all entities that intersect the given point, ordered front-to-back.
   *
   * @param point Point to find intersecting entities for
   * @param mode Hit eligibility policy.
   */
  std::vector<Entity> findAllIntersecting(const Vector2d& point,
                                          HitTestMode mode = HitTestMode::SvgPointerEvents);

  /**
   * Map a rendered entity to the document-tree element that receives its pointer events.
   *
   * Content instantiated by `<use>` lives in a shadow tree that is rebuilt with the render tree.
   * As in SVG 2, events on those instances target the `<use>` element, the outermost one for
   * nested instances. Offscreen content (patterns, masks, markers and `feImage` fragments) is
   * painted as part of the element that references it and receives no events.
   *
   * @param registry Registry holding \p entity.
   * @param entity Rendered entity, such as a hit returned by \ref findIntersecting.
   * @return The document-tree entity, or `entt::null` for offscreen content.
   */
  static Entity EventTargetEntity(const Registry& registry, Entity entity);

  /**
   * Find all entities whose world-space bounds intersect the given rectangle,
   * ordered front-to-back. Uses the spatial grid for acceleration when available.
   *
   * @param rect Rectangle in world coordinates to test intersection against
   */
  std::vector<Entity> findIntersectingRect(const Box2d& rect);

  /**
   * Get the world-space bounding box of an entity (fill bounds only, not including stroke).
   *
   * @param entity Entity to get bounds for
   * @return The world-space AABB, or std::nullopt if the entity has no shape
   */
  std::optional<Box2d> getWorldBounds(Entity entity);

  /**
   * Invalidate the rendering tree, forcing it to be recreated on the next render.
   */
  void invalidateRenderTree();

  /// Result from createFeImageShadowTree.
  struct FeImageSubtreeResult {
    Entity firstEntity;  //!< First entity in the shadow subtree.
    Entity lastEntity;   //!< Last entity in the shadow subtree.
  };

  /**
   * Create a shadow tree for an feImage fragment reference, allowing elements inside `<defs>`
   * to be rendered offscreen.
   */
  std::optional<FeImageSubtreeResult> createFeImageShadowTree(Entity hostEntity,
                                                              Entity targetEntity, bool verbose);

  /**
   * Set initial context-fill and context-stroke values for sub-document rendering.
   * When a `<use>` element references an external SVG, the `<use>` element's resolved fill
   * and stroke become the initial context-fill/context-stroke in the sub-document.
   *
   * @param fill Initial context-fill value.
   * @param stroke Initial context-stroke value.
   */
  void setInitialContextPaint(const ResolvedPaintServer& fill, const ResolvedPaintServer& stroke);

  /**
   * Clear previously set context-fill and context-stroke values.
   * Call after rendering a sub-document to prevent stale context paint from leaking to subsequent
   * renders of the same cached sub-document handle.
   */
  void clearInitialContextPaint();

  /// Owned paint state saved and restored under a sub-document's write guard.
  using InitialPaintState =
      std::pair<std::optional<ResolvedPaintServer>, std::optional<ResolvedPaintServer>>;
  /// Return the owned context paints for restoration after a scoped external-use render.
  InitialPaintState initialPaintState() const {
    return {initialContextFill_, initialContextStroke_};
  }
  /// Restore the state that preceded a scoped external-use occurrence.
  void restoreInitialPaintState(InitialPaintState state);

private:
  /**
   * Test whether a single entity is hit by the given point, applying pointer-events rules.
   *
   * @param entity Entity to test
   * @param point Point in world coordinates
   * @param mode Hit eligibility policy.
   * @return true if the entity is hit
   */
  bool hitTestEntity(Entity entity, const Vector2d& point, HitTestMode mode);
  /// Prepare metadata and a path borrowed only until the next document mutation.
  bool prepareHitGeometry(Entity entity, HitGeometry& geometry, const Path*& path,
                          HitTestMode mode = HitTestMode::SvgPointerEvents);

  /**
   * Create all computed parts of the tree, evaluating styles and creating shadow trees.
   *
   * @param warningSink Sink to collect warnings.
   */
  void createComputedComponents(ParseWarningSink& warningSink);

  /**
   * Creates all rendering instances for the document, the final step before it can be rendered.
   *
   * @param verbose If true, enable verbose logging.
   */
  void instantiateRenderTreeWithPrecomputedTree(bool verbose);

private:
  /// Reference to the registry containing the render tree.
  Registry& registry_;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)

  /// Initial context-fill value for sub-document rendering.
  std::optional<ResolvedPaintServer> initialContextFill_;
  /// Initial context-stroke value for sub-document rendering.
  std::optional<ResolvedPaintServer> initialContextStroke_;
};

}  // namespace donner::svg::components
