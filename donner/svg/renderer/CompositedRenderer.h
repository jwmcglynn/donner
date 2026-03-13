#pragma once
/// @file

#include <memory>
#include <optional>
#include <vector>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/renderer/CompositingLayer.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/resources/ImageResource.h"

namespace donner::svg {

/**
 * Composited renderer that caches static layers as pixmaps and only re-renders
 * dirty (animated/edited) layers each frame. Composes all layers in paint order
 * to produce the final image.
 *
 * Usage:
 * @code
 * CompositedRenderer compositor(renderer);
 * compositor.prepare(document, animatedEntities);
 *
 * // First render: rasterizes all layers.
 * compositor.renderFrame();
 * auto bitmap = compositor.takeSnapshot();
 *
 * // Animation loop: advance time, invalidate, re-render.
 * document.setDocumentTime(0.5);
 * compositor.invalidateAnimatedLayers();
 * compositor.renderFrame();
 * @endcode
 */
class CompositedRenderer {
public:
  /**
   * Construct a composited renderer that delegates to the given backend.
   *
   * @param renderer Backend renderer used for both per-layer rasterization and
   *                 final composition.
   * @param verbose Enable verbose logging.
   */
  explicit CompositedRenderer(RendererInterface& renderer, bool verbose = false);

  ~CompositedRenderer();

  // Non-copyable, non-movable (owns offscreen renderers).
  CompositedRenderer(const CompositedRenderer&) = delete;
  CompositedRenderer& operator=(const CompositedRenderer&) = delete;
  CompositedRenderer(CompositedRenderer&&) = delete;
  CompositedRenderer& operator=(CompositedRenderer&&) = delete;

  /**
   * Prepare the document for composited rendering. Instantiates the render tree
   * and decomposes it into layers based on the promoted entities.
   *
   * @param document The SVG document to render.
   * @param promotedEntities Entities to promote to dynamic layers.
   * @param reason Reason for promotion (Animation, Selection, etc.).
   */
  void prepare(SVGDocument& document, const std::vector<Entity>& promotedEntities,
               CompositingLayer::Reason reason = CompositingLayer::Reason::Animation);

  /**
   * Render all dirty layers and compose the final image.
   * On first call, all layers are dirty. On subsequent calls, only layers
   * explicitly marked dirty are re-rendered.
   */
  void renderFrame();

  /**
   * Compose the final image from cached layer pixmaps without rasterizing
   * any layers, even if they are dirty. This is the fast path for interactive
   * editing: apply transform-only changes (translation, rotation, scale) via
   * setLayerTransform() and then call composeOnly() for immediate visual
   * feedback at 60 FPS.
   *
   * Dirty layers retain their stale cached pixmaps and remain marked dirty
   * for the next renderFrame() call.
   */
  void composeOnly();

  /**
   * Mark a specific layer as needing re-rasterization.
   *
   * @param layerId The layer ID to mark dirty.
   */
  void markLayerDirty(uint32_t layerId);

  /**
   * Mark all layers as dirty (e.g., on viewport resize).
   */
  void markAllDirty();

  /**
   * Mark the layer containing the given entity as dirty.
   * Uses the entity's LayerMembershipComponent to find the containing layer.
   *
   * @param entity The entity that changed.
   * @return True if the entity's layer was found and marked dirty.
   */
  bool markEntityDirty(Entity entity);

  /**
   * Scan the document for entities with active animations and mark their
   * containing layers as dirty. Call this after advancing the document time
   * and before renderFrame().
   *
   * @return The number of layers marked dirty.
   */
  uint32_t invalidateAnimatedLayers();

  /**
   * Set the composition transform for a layer. This transform is applied at
   * composition time when drawing the cached pixmap, without re-rasterizing
   * the layer.
   *
   * Use this for interactive operations like dragging (translation), rotation,
   * or scaling where immediate visual feedback is needed. The stale pixmap is
   * reused with the new transform applied.
   *
   * @param layerId The layer to transform.
   * @param transform The composition transform to apply.
   */
  void setLayerTransform(uint32_t layerId, const Transformd& transform);

  /**
   * Set the composition transform for the layer containing the given entity.
   *
   * @param entity The entity whose layer should be transformed.
   * @param transform The composition transform to apply.
   * @return True if the entity's layer was found.
   */
  bool setEntityLayerTransform(Entity entity, const Transformd& transform);

  /**
   * Set the composition opacity for a layer. This opacity is applied at
   * composition time when drawing the cached pixmap, without re-rasterizing
   * the layer. Use this for animated group opacity.
   *
   * @param layerId The layer to set opacity for.
   * @param opacity The opacity value (0.0 = fully transparent, 1.0 = opaque).
   */
  void setLayerOpacity(uint32_t layerId, double opacity);

  /**
   * Set the composition opacity for the layer containing the given entity.
   *
   * @param entity The entity whose layer opacity should be changed.
   * @param opacity The opacity value.
   * @return True if the entity's layer was found.
   */
  bool setEntityLayerOpacity(Entity entity, double opacity);

  /**
   * Find the layer ID that contains the given entity.
   *
   * @param entity The entity to look up (data or render entity).
   * @return The layer ID, or nullopt if not found.
   */
  [[nodiscard]] std::optional<uint32_t> findLayerForEntity(Entity entity) const;

  /**
   * Capture a snapshot of the final composited image.
   */
  [[nodiscard]] RendererBitmap takeSnapshot() const;

  /**
   * Get the current layer decomposition result.
   */
  [[nodiscard]] const LayerDecompositionResult& layers() const { return layers_; }

  /**
   * Pre-render dirty layers for a future document time. This rasterizes the
   * predicted next frame's content into a separate buffer. Call swapPredicted()
   * to promote the pre-rendered content to the active layer cache.
   *
   * Usage for animation:
   * @code
   * // Current frame at time T:
   * compositor.renderFrame();
   * auto current = compositor.takeSnapshot();
   *
   * // Pre-render next frame (T + dt):
   * document.setDocumentTime(T + dt);
   * compositor.invalidateAnimatedLayers();
   * compositor.renderPredicted();
   *
   * // When displaying the next frame:
   * compositor.swapPredicted();
   * compositor.composeOnly();  // Fast: just compose pre-rendered layers.
   * @endcode
   *
   * @return The number of layers pre-rendered.
   */
  uint32_t renderPredicted();

  /**
   * Promote pre-rendered layer content to the active cache. After this call,
   * the pre-rendered pixmaps become the current layer pixmaps, and layers that
   * were pre-rendered are marked clean.
   */
  void swapPredicted();

  /**
   * Per-frame statistics for monitoring compositing performance.
   */
  struct FrameStats {
    /// Number of layers rasterized in the last renderFrame() call.
    uint32_t layersRasterized = 0;
    /// Number of layers reused from cache (not dirty).
    uint32_t layersReused = 0;
    /// Number of offscreen renderer instances reused from pool.
    uint32_t offscreenPoolHits = 0;
    /// Number of ImageResource objects reused from cache during composition.
    uint32_t imagePoolHits = 0;
  };

  /**
   * Get statistics from the most recent renderFrame() call.
   */
  [[nodiscard]] const FrameStats& lastFrameStats() const { return lastFrameStats_; }

private:
  /// Cached state for each layer (indexed by layer.id).
  struct LayerCache {
    RendererBitmap bitmap;
    bool dirty = true;

    /// Pooled offscreen renderer — reused across frames to avoid allocation.
    std::unique_ptr<RendererInterface> offscreen;

    /// Cached ImageResource for composition — rebuilt only after rasterization.
    ImageResource cachedImage;
    bool imageDirty = true;
  };

  /// Rasterize a single layer into its cached pixmap.
  void rasterizeLayer(CompositingLayer& layer);

  /// Rebuild the cached ImageResource from a layer's bitmap.
  void rebuildCachedImage(LayerCache& cache);

  /// Compose all cached layer pixmaps into the final output.
  void composeLayers();

  RendererInterface& renderer_;
  bool verbose_ = false;

  SVGDocument* document_ = nullptr;
  LayerDecompositionResult layers_;

  std::vector<LayerCache> layerCaches_;

  /// Canvas size from the document.
  Vector2i canvasSize_;

  /// Last promotion set and reason, for incremental reassignment detection.
  std::vector<Entity> lastPromotedEntities_;
  CompositingLayer::Reason lastReason_ = CompositingLayer::Reason::Static;

  /// True when the composited output needs to be re-assembled (layers rasterized,
  /// composition transform/opacity changed). When false, the renderer's surface
  /// already contains the correct output from the previous composition.
  bool compositionDirty_ = true;

  /// Stats from the most recent renderFrame().
  FrameStats lastFrameStats_;

  /// Pre-rendered layer pixmaps for predictive rendering.
  struct PredictedCache {
    std::vector<RendererBitmap> bitmaps;
    std::vector<bool> valid;  ///< Which layers have pre-rendered content.
  };
  PredictedCache predicted_;
};

}  // namespace donner::svg
