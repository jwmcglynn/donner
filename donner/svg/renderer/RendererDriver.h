#pragma once
/// @file

#include <unordered_set>

#include "donner/svg/SVGDocument.h"
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
   * @param baseTransform Transform applied to all entities (e.g., layer-local offset).
   */
  void drawEntityRange(Registry& registry, Entity firstEntity, Entity lastEntity,
                       const RenderViewport& viewport, const Transform2d& baseTransform);

  /**
   * Capture a snapshot from the underlying backend after rendering.
   */
  [[nodiscard]] RendererBitmap takeSnapshot() const;

private:
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
  Transform2d layerBaseTransform_;
  Vector2i renderingSize_ = Vector2i::Zero();

  /// Recursion guard for feImage fragment rendering. Tracks entity IDs currently being rendered
  /// as feImage fragments to prevent infinite recursion. Shared across nested RendererDriver
  /// instances via pointer.
  std::unordered_set<entt::id_type>* feImageFragmentGuard_ = nullptr;
};

}  // namespace donner::svg
