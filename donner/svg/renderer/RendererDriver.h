#pragma once
/// @file

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/shape/ComputedPathComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/core/MarkerOrient.h"
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
  void renderMask(RenderingInstanceView& view, Registry& registry,
                  const components::RenderingInstanceComponent& instance,
                  const components::ResolvedMask& mask);
  void renderPattern(RenderingInstanceView& view, Registry& registry,
                     const components::RenderingInstanceComponent& instance,
                     const components::PaintResolvedReference& ref, bool forStroke);

  struct DeferredPop {
    Entity lastEntity{};
    bool hasViewportClip = false;
    bool hasIsolatedLayer = false;
    bool hasFilterLayer = false;
    bool hasEntityClip = false;
    bool hasMask = false;
  };

  RendererInterface& renderer_;
  bool verbose_ = false;
  std::vector<DeferredPop> subtreeMarkers_;
  Transformd layerBaseTransform_;
  Vector2i renderingSize_;  ///< Canvas size in pixels, set during draw().
};

}  // namespace donner::svg
