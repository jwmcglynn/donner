#pragma once
/// @file

#include <memory>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::geode {
class GeodeDevice;
}

namespace donner::svg {

/**
 * Rasterize an element subtree through an isolated instance of a specific
 * renderer implementation. This is the backend-explicit counterpart to
 * `Renderer::renderElementToBitmap` for callers that intentionally choose a
 * CPU or GPU renderer.
 *
 * @param renderer Backend instance used to rasterize the subtree.
 * @param element Root element of the subtree to rasterize.
 * @param sizePx Output bitmap dimensions in pixels.
 * @return The rasterized subtree bitmap.
 */
[[nodiscard]] RendererBitmap RenderElementToBitmap(RendererInterface& renderer, SVGElement element,
                                                   Vector2i sizePx);

/**
 * Backend-agnostic renderer that resolves to the active build backend (Skia or tiny-skia).
 *
 * Typical usage:
 * ```cpp
 * Renderer renderer;
 * renderer.draw(document);
 * renderer.save("output.png");
 * ```
 *
 * The `draw()` method handles the full rendering pipeline internally: computing styles, building
 * the render tree, and rasterizing. For frame-based rendering (e.g., in a viewer), use
 * `beginFrame()` / `endFrame()` directly via the \ref RendererInterface API.
 */
class Renderer : public RendererInterface {
public:
  /**
   * Creates a renderer for the active backend.
   *
   * @param verbose If true, enables backend-specific verbose logging.
   */
  explicit Renderer(bool verbose = false);

  /**
   * Creates a renderer for the active backend using an externally-owned
   * Geode device when the Geode backend is selected.
   *
   * Non-Geode backends ignore \p device and construct their normal backend.
   *
   * @param device Shared Geode device.
   * @param verbose If true, enables backend-specific verbose logging.
   */
  explicit Renderer(std::shared_ptr<geode::GeodeDevice> device, bool verbose = false);

  /// Destructor.
  ~Renderer() override;

  /// Move constructor.
  Renderer(Renderer&&) noexcept;

  /// Move assignment operator.
  Renderer& operator=(Renderer&&) noexcept;

  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;

  /**
   * Draws the SVG document using the active backend.
   *
   * @param document The SVG document to render.
   */
  void draw(SVGDocument& document) override;

  /**
   * Rasterize a single \ref SVGElement and its descendant subtree to an RGBA
   * bitmap fitted into a target box.
   *
   * This renders just the given element + its descendants through an isolated
   * offscreen instance of this renderer, scaled and centered into @p sizePx with
   * a transparent background. The requested size is a maximum thumbnail extent:
   * the returned bitmap dimensions are computed from the element's nonzero-
   * opacity painted geometry bounds and may be narrower or shorter than
   * @p sizePx. Layers whose visible bounds cover the root SVG viewBox fit the
   * canvas-visible bounds into the preview cell. The element's own paint and
   * transform are honored. The bounds are clipped to the root SVG viewBox when
   * present so thumbnails show the document-canvas-visible part of a layer;
   * other ancestor clips and opacity are intentionally ignored so the preview
   * shows the element's own artwork rather than how it happens to be clipped in
   * context.
   *
   * The element's owning document is prepared for rendering under write access
   * for this isolated draw, then its pending render invalidation state is
   * restored so previews do not consume dirty flags needed by another renderer.
   *
   * @param element Element whose subtree should be rasterized.
   * @param sizePx Maximum bitmap dimensions in device pixels. Both axes must be
   *   positive; otherwise an empty bitmap is returned.
   * @return The rendered RGBA bitmap with a transparent background, or an empty
   *   \ref RendererBitmap when the element has no renderable geometry or the
   *   inputs are degenerate.
   */
  [[nodiscard]] RendererBitmap renderElementToBitmap(SVGElement element, Vector2i sizePx);

  /**
   * Begins a render pass for the given viewport.
   *
   * @param viewport The viewport dimensions for the render pass.
   */
  void beginFrame(const RenderViewport& viewport) override;

  /// Completes the current render pass.
  void endFrame() override;

  /**
   * Sets the absolute transform, replacing the current matrix.
   *
   * @param transform The transform to apply.
   */
  void setTransform(const Transform2d& transform) override;

  /**
   * Pushes a relative transform.
   *
   * @param transform The transform to compose with the current matrix.
   */
  void pushTransform(const Transform2d& transform) override;

  /// Pops the most recent transform.
  void popTransform() override;

  /**
   * Pushes a clip rect or clip path.
   *
   * @param clip The resolved clip to apply.
   */
  void pushClip(const ResolvedClip& clip) override;

  /// Pops the most recent clip.
  void popClip() override;

  /**
   * Pushes an isolated compositing layer.
   *
   * @param opacity Group opacity applied when the layer is composited back.
   * @param blendMode Mix-blend-mode applied when the layer is composited back.
   */
  void pushIsolatedLayer(double opacity, MixBlendMode blendMode) override;

  /// Pops the most recent isolated layer.
  void popIsolatedLayer() override;

  /**
   * Pushes a filter layer.
   *
   * @param filterGraph The filter graph to apply when the layer is popped.
   * @param filterRegion Optional filter region bounds in user space.
   */
  void pushFilterLayer(const components::FilterGraph& filterGraph,
                       const std::optional<Box2d>& filterRegion) override;

  /// Pops the most recent filter layer.
  void popFilterLayer() override;

  /**
   * Begins mask rendering.
   *
   * @param maskBounds Optional mask bounds clip.
   */
  void pushMask(const std::optional<Box2d>& maskBounds) override;

  /// Switches from mask rendering to masked content rendering.
  void transitionMaskToContent() override;

  /// Pops the current mask and composites the masked content.
  void popMask() override;

  /**
   * Begins pattern tile recording.
   *
   * @param tileRect Tile bounds in pattern space.
   * @param targetFromPattern Transform from pattern tile space to target space.
   */
  [[nodiscard]] bool beginPatternTile(const Box2d& tileRect,
                                      const Transform2d& targetFromPattern) override;

  /**
   * Ends pattern recording and stores the resulting pattern paint.
   *
   * @param forStroke If true, store as stroke paint, otherwise fill paint.
   */
  void endPatternTile(bool forStroke) override;

  /**
   * Sets the active paint parameters for subsequent draw calls.
   *
   * @param paint The resolved paint state.
   */
  void setPaint(const PaintParams& paint) override;

  /**
   * Draws an arbitrary path.
   *
   * @param path The path to draw.
   * @param stroke Stroke configuration for the path.
   */
  void drawPath(const PathShape& path, const StrokeParams& stroke) override;

  /**
   * Draws an axis-aligned rectangle.
   *
   * @param rect The rectangle bounds.
   * @param stroke Stroke configuration for the rectangle.
   */
  void drawRect(const Box2d& rect, const StrokeParams& stroke) override;

  /**
   * Draws an ellipse.
   *
   * @param bounds The ellipse bounds.
   * @param stroke Stroke configuration for the ellipse.
   */
  void drawEllipse(const Box2d& bounds, const StrokeParams& stroke) override;

  /**
   * Draws an image resource.
   *
   * @param image The image resource to draw.
   * @param params Image placement parameters.
   */
  void drawImage(const ImageResource& image, const ImageParams& params) override;

  /**
   * Draws a CPU-readable bitmap into the given target rectangle.
   *
   * @param bitmap The bitmap to draw.
   * @param params Image placement parameters.
   */
  void drawBitmap(const RendererBitmap& bitmap, const ImageParams& params) override;

  /// Draws a backend-owned texture snapshot when the active backend supports it.
  bool drawTextureSnapshot(const RendererTextureSnapshot& texture, const Box2d& targetRect,
                           double opacity = 1.0, bool pixelated = false) override;

  /**
   * Draws shaped text.
   *
   * @param registry ECS registry used for resolving paint references.
   * @param text The shaped text runs.
   * @param params Text styling parameters.
   */
  void drawText(Registry& registry, const components::ComputedTextComponent& text,
                const TextParams& params) override;

  /**
   * Captures a CPU-readable snapshot of the current frame.
   *
   * @return A snapshot of the rendered frame.
   */
  [[nodiscard]] RendererBitmap takeSnapshot() const override;

  /// Captures a backend-owned GPU texture snapshot when the active backend supports it.
  [[nodiscard]] std::shared_ptr<const RendererTextureSnapshot> takeTextureSnapshot() override;

  /// Returns true when this backend requires direct texture presentation.
  [[nodiscard]] bool requiresTextureSnapshotPresentation() const override;

  /// Creates an offscreen renderer of the active backend type.
  [[nodiscard]] std::unique_ptr<RendererInterface> createOffscreenInstance() const override;

  /// Forwards \ref RendererInterface::setDebugGeometryOverlay to the
  /// active backend (no-op for backends without a debug overlay).
  void setDebugGeometryOverlay(bool enabled) override;

  /// Whether the active backend's geometry debug overlay is enabled.
  [[nodiscard]] bool debugGeometryOverlay() const override;

  /**
   * Saves the last rendered frame to a PNG file.
   *
   * @param filename The output PNG filename.
   * @return True if the file was written.
   */
  bool save(const char* filename);

  /**
   * Returns the rendered width in pixels.
   *
   * @return The rendered width.
   */
  [[nodiscard]] int width() const override;

  /**
   * Returns the rendered height in pixels.
   *
   * @return The rendered height.
   */
  [[nodiscard]] int height() const override;

private:
  std::unique_ptr<RendererInterface> impl_;
};

}  // namespace donner::svg
