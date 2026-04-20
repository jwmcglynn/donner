#pragma once
/// @file

#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/filter/FilterGraph.h"
#include "donner/svg/components/shape/ComputedPathComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/core/MarkerOrient.h"
#include "donner/svg/core/PreserveAspectRatio.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/common/RenderingInstanceView.h"

namespace donner::svg {

/**
 * Backend-agnostic renderer driver that prepares documents for rendering and
 * emits drawing commands through a \ref RendererInterface implementation.
 */
class RendererDriver {
public:
  /**
   * Create a renderer driver that will forward traversal output to the given
   * backend implementation.
   */
  explicit RendererDriver(RendererInterface& renderer, bool verbose = false);

  /**
   * Render the given \ref SVGDocument using the configured backend.
   */
  void draw(SVGDocument& document);

  /**
   * Render a range of entities from an already-prepared document's render tree.
   * The document must have been prepared via RendererUtils::prepareDocumentForRendering()
   * before calling this method.
   *
   * @param registry The registry containing the prepared render tree.
   * @param firstEntity First entity in the range to render (inclusive).
   * @param lastEntity Last entity in the range to render (inclusive).
   * @param viewport Viewport for the render pass.
   * @param surfaceFromCanvas Transform that maps canvas coords to the render
   *     surface (`Translate(-topLeft)` for tight-bounded compositor segments,
   *     identity for full-canvas layers/segments). Named with `destFromSource`
   *     convention; composed with each entity's `worldFromEntityTransform` as
   *     `worldFromEntityTransform * surfaceFromCanvas` (donner's left-first
   *     `operator*`: "apply entity-to-canvas, then canvas-to-surface").
   */
  void drawEntityRange(Registry& registry, Entity firstEntity, Entity lastEntity,
                       const RenderViewport& viewport, const Transform2d& surfaceFromCanvas);

  /**
   * Compute the canvas-space bounding box of every pixel a subsequent
   * `drawEntityRange(registry, firstEntity, lastEntity, viewport,
   * baseTransform)` call would write to. Runs the same entity traversal
   * as `drawEntityRange` but with a bounds-accumulating visitor; no
   * side effects on the renderer or registry.
   *
   * The bounds include the effect of:
   *   - Per-entity transforms (including the base transform).
   *   - Filter regions (via `computeFilterRegion`; the filter's output
   *     rectangle is taken as the entity subtree's contribution).
   *   - Stroke widths on stroked paths (expanded by `strokeWidth / 2`
   *     plus a miter margin).
   *   - Isolated layers — child bounds accumulate into the parent.
   *   - Clip-rects (intersect, shrink only).
   *
   * Returns `std::nullopt` when:
   *   - The range is empty / renders nothing.
   *   - Any entity uses a bound-expander the visitor doesn't yet model
   *     precisely (text, markers, masks, patterns). Callers must treat
   *     `nullopt` as "fall back to full-canvas render", NOT as "empty
   *     segment". The design doc at `docs/design_docs/0027-tight_bounded_
   *     segments.md` tracks which cases are pending.
   *
   * Safe to call on a `RendererDriver` whose renderer holds persistent
   * state — the traversal never invokes renderer methods.
   */
  [[nodiscard]] std::optional<Box2d> computeEntityRangeBounds(
      Registry& registry, Entity firstEntity, Entity lastEntity,
      const RenderViewport& viewport, const Transform2d& surfaceFromCanvas);

  /**
   * Capture a snapshot from the underlying backend after rendering.
   */
  [[nodiscard]] RendererBitmap takeSnapshot() const;

private:
  /**
   * Resolve and pre-render the filter graph for every entity with a resolved filter in `entities`,
   * caching each result in \ref preparedFilterGraphs_. Must be called BEFORE any `traverse` pass
   * so that `traverse` does not mutate \ref components::RenderingInstanceComponent storage while
   * iterating it (which would invalidate iterators and references held by the traverse loop).
   *
   * `entities` is the ordered snapshot of main-tree entities that will be rendered. The pre-pass
   * iterates this list rather than a live view so it is robust against storage mutation caused by
   * `preRenderFeImageFragments` (which creates offscreen shadow trees and globally sorts the
   * `RenderingInstanceComponent` pool).
   */
  void prepareFilterGraphs(Registry& registry, std::span<const Entity> entities);

  /// Fetch a prepared filter graph for an entity, if any. Returns nullptr if the entity has no
  /// filter or if the pre-pass didn't record one for it.
  const components::FilterGraph* preparedFilterGraphFor(Entity entity) const;

  /// Fetch the prepared filter region for an entity, if any.
  std::optional<Box2d> preparedFilterRegionFor(Entity entity) const;

  void traverse(RenderingInstanceView& view, Registry& registry);
  void traverseRange(RenderingInstanceView& view, Registry& registry, Entity startEntity,
                     Entity endEntity);
  void skipUntil(RenderingInstanceView& view, Entity endEntity);
  void drawMarkers(RenderingInstanceView& view, Registry& registry,
                   const components::RenderingInstanceComponent& instance,
                   const components::ComputedPathComponent& path,
                   const components::ComputedStyleComponent& style);
  void drawMarker(RenderingInstanceView& view, Registry& registry,
                  const components::RenderingInstanceComponent& instance,
                  const components::ResolvedMarker& marker, const Vector2d& vertexPosition,
                  const Vector2d& direction, MarkerOrient::MarkerType markerOrientType,
                  const components::ComputedStyleComponent& style);
  int renderMask(RenderingInstanceView& view, Registry& registry,
                 const components::RenderingInstanceComponent& instance,
                 const components::ResolvedMask& mask);
  void renderPattern(RenderingInstanceView& view, Registry& registry,
                     const components::RenderingInstanceComponent& instance,
                     const components::PaintResolvedReference& ref, bool forStroke);
  void drawSubDocument(SVGDocument& subDocument, const Box2d& viewportBounds,
                       const PreserveAspectRatio& aspectRatio, double opacity,
                       const Transform2d& parentAbsoluteTransform);
  void drawSubDocumentElement(SVGDocument& subDocument, std::string_view fragmentId,
                              const Transform2d& parentAbsoluteTransform, double opacity);
  void preRenderSvgFeImages(components::FilterGraph& filterGraph);
  void preRenderFeImageFragments(components::FilterGraph& filterGraph, Registry& registry,
                                 Entity hostEntity, const std::optional<Box2d>& filterRegion);
  static void setSubDocumentContextPaint(SVGDocument& subDocument,
                                         const components::ResolvedPaintServer& contextFill,
                                         const components::ResolvedPaintServer& contextStroke);
  static void clearSubDocumentContextPaint(SVGDocument& subDocument);

  struct DeferredPop {
    Entity lastEntity{};
    bool hasViewportClip = false;
    bool hasIsolatedLayer = false;
    bool hasFilterLayer = false;
    bool hasEntityClip = false;
    int maskDepth = 0;
  };

  RendererInterface& renderer_;
  bool verbose_ = false;
  std::vector<DeferredPop> subtreeMarkers_;
  /// Post-entity transform composed onto the CTM for every entity drawn via
  /// `drawEntityRange`/`traverse`. Named with `destFromSource` convention: it
  /// maps canvas coords to the render surface we're drawing into. For the
  /// compositor's tight-bounded segment path this is `Translate(-topLeft)`
  /// (canvas → tight-bitmap). For sub-document rendering it's a compound
  /// `subDocFromLocal * parentAbsoluteTransform * canvasFromDoc` that, when
  /// post-composed with the sub-doc entity's own `worldFromEntity` (named
  /// `worldFromEntityTransform` for historical reasons — see
  /// `RenderingInstanceComponent.h`), yields the correct device CTM.
  ///
  /// Composition order matters. `Transform2d::operator*` is left-first
  /// (`A * B` means "apply A, then B"), so the correct CTM is
  /// `worldFromEntityTransform * surfaceFromCanvasTransform_`: first map
  /// entity-local to canvas via `worldFromEntityTransform`, then canvas to
  /// surface via this. Swapping the two silently mis-renders rotated
  /// paths (translations commute, rotations don't) — see
  /// `TightBoundsRotatedEllipseWithRotatingGradient`.
  Transform2d surfaceFromCanvasTransform_;
  Vector2i renderingSize_ = Vector2i::Zero();

  /// Recursion guard for feImage fragment rendering. Tracks entity IDs currently being rendered
  /// as feImage fragments to prevent infinite recursion. Shared across nested RendererDriver
  /// instances via pointer.
  std::unordered_set<entt::id_type>* feImageFragmentGuard_ = nullptr;

  /// Filter graphs resolved and pre-rendered by \ref prepareFilterGraphs. Keyed by the entity
  /// whose filter produced the graph. Populated in a pre-pass before \ref traverse so the main
  /// traversal never mutates \ref components::RenderingInstanceComponent storage mid-iteration.
  std::unordered_map<Entity, components::FilterGraph> preparedFilterGraphs_;

  /// Filter regions resolved alongside \ref preparedFilterGraphs_. Stored separately because
  /// `computeFilterRegion` needs the (live) `RenderingInstanceComponent` reference, and the region
  /// is otherwise just an `std::optional<Box2d>` value pulled back out per-entity in traverse.
  std::unordered_map<Entity, std::optional<Box2d>> preparedFilterRegions_;
};

}  // namespace donner::svg
