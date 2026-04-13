#pragma once
/// @file

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/base/Transform.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/compositor/CompositorLayer.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::svg::compositor {

/// Maximum number of compositor layers that can be simultaneously active.
inline constexpr int kMaxCompositorLayers = 32;

/// Maximum total memory budget for compositor layer bitmaps, in bytes.
inline constexpr size_t kMaxCompositorMemoryBytes = 256 * 1024 * 1024;

/**
 * Controls compositor layer promotion/demotion and orchestrates composited rendering.
 *
 * The compositor splits the document into layers: one root layer (everything not promoted) and
 * zero or more promoted layers (one per promoted entity subtree). During interactive drag, only
 * the dragged layer's composition transform changes — no re-rasterization is needed for
 * translation-only drags when the layer's content hasn't changed.
 *
 * Usage:
 * ```cpp
 * CompositorController compositor(document, renderer);
 *
 * // Promote drag target
 * compositor.promoteEntity(dragTarget);
 *
 * // During drag — update composition transform, render composited frame
 * compositor.setLayerCompositionTransform(dragTarget, Transform2d::Translate(dx, dy));
 * compositor.renderFrame(viewport);
 *
 * // When drag ends
 * compositor.demoteEntity(dragTarget);
 * ```
 */
class CompositorController {
public:
  /**
   * Construct a compositor controller.
   *
   * @param document The SVG document to composite.
   * @param renderer The renderer backend to use for rasterization and composition.
   */
  CompositorController(SVGDocument& document, RendererInterface& renderer);

  /// Destructor.
  ~CompositorController();

  // Non-copyable, movable.
  CompositorController(const CompositorController&) = delete;
  CompositorController& operator=(const CompositorController&) = delete;
  CompositorController(CompositorController&&) noexcept;
  CompositorController& operator=(CompositorController&&) noexcept;

  /**
   * Promote an entity to its own compositor layer.
   *
   * The entity and its subtree will be rasterized into a separate bitmap. During composition,
   * the layer bitmap is blitted with its composition transform, avoiding re-rasterization of
   * the rest of the scene.
   *
   * @param entity The entity to promote.
   * @return true if promotion succeeded, false if the layer limit or memory budget was reached.
   */
  bool promoteEntity(Entity entity);

  /**
   * Demote a previously promoted entity back to the root layer.
   *
   * The entity's `LayerMembershipComponent` is removed and the layer is destroyed. The root
   * layer is marked dirty to include the demoted entity on the next render.
   *
   * @param entity The entity to demote. No-op if not currently promoted.
   */
  void demoteEntity(Entity entity);

  /**
   * Returns true if the given entity is currently promoted to its own layer.
   *
   * @param entity The entity to check.
   */
  [[nodiscard]] bool isPromoted(Entity entity) const;

  /**
   * Set the composition transform for a promoted entity's layer.
   *
   * For translation-only transforms, the layer bitmap is blitted at the new offset without
   * re-rasterization. Non-translation transforms mark the layer dirty for re-rasterization.
   *
   * @param entity The promoted entity.
   * @param transform The composition transform to apply during blitting.
   */
  void setLayerCompositionTransform(Entity entity, const Transform2d& transform);

  /**
   * Returns the current composition transform for a promoted entity, or identity if not promoted.
   *
   * @param entity The entity to query.
   */
  [[nodiscard]] Transform2d compositionTransformOf(Entity entity) const;

  /**
   * Prepare and render a composited frame.
   *
   * This method:
   * 1. Prepares the document (computes styles, layout, render tree) if needed.
   * 2. Checks dirty flags on promoted entities and marks their layers dirty.
   * 3. Re-rasterizes dirty layers via `createOffscreenInstance()` + `drawEntityRange()`.
   * 4. Rasterizes the root layer (everything not in a promoted layer).
   * 5. Composes all layers in paint order via `drawImage()`.
   *
   * @param viewport The viewport for the render pass.
   */
  void renderFrame(const RenderViewport& viewport);

  /**
   * Returns the number of currently active layers (excluding the root layer).
   */
  [[nodiscard]] size_t layerCount() const;

  /**
   * Returns the total memory used by all layer bitmaps, in bytes.
   */
  [[nodiscard]] size_t totalBitmapMemory() const;

  /**
   * Returns a reference to the underlying SVG document.
   */
  [[nodiscard]] SVGDocument& document() { return *document_; }

private:
  /// Find the layer for a given entity, or nullptr if not promoted.
  CompositorLayer* findLayer(Entity entity);
  const CompositorLayer* findLayer(Entity entity) const;

  /// Rasterize a single promoted layer into its bitmap cache.
  void rasterizeLayer(CompositorLayer& layer, const RenderViewport& viewport);

  /// Rasterize the root layer (full document render for v1).
  void rasterizeRootLayer(const RenderViewport& viewport);

  /// Compose all layers onto the main render target.
  void composeLayers(const RenderViewport& viewport);

  /// Check dirty flags on promoted entities and mark affected layers.
  void consumeDirtyFlags();

  /// Compute the entity range for a promoted entity.
  static std::pair<Entity, Entity> computeEntityRange(Registry& registry, Entity entity);

  /// Next layer ID to assign.
  uint32_t nextLayerId_ = 1;

  SVGDocument* document_ = nullptr;
  RendererInterface* renderer_ = nullptr;
  std::vector<CompositorLayer> layers_;

  /// Cached root layer bitmap (everything not in a promoted layer).
  RendererBitmap rootBitmap_;
  bool rootDirty_ = true;
};

}  // namespace donner::svg::compositor
