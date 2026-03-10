#pragma once
/// @file

#include <memory>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::svg {

class RendererImplementation;

/**
 * Backend-agnostic renderer that resolves to the active build backend.
 *
 * Clients should prefer this type when they do not need backend-specific APIs.
 */
class Renderer : public RendererInterface {
public:
  /**
   * Creates a renderer for the active backend.
   *
   * @param verbose If true, enables backend-specific verbose logging.
   */
  explicit Renderer(bool verbose = false);

  /// Destructor.
  ~Renderer();

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
  void draw(SVGDocument& document);

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
  void setTransform(const Transformd& transform) override;

  /**
   * Pushes a relative transform.
   *
   * @param transform The transform to compose with the current matrix.
   */
  void pushTransform(const Transformd& transform) override;

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
   */
  void pushIsolatedLayer(double opacity) override;

  /// Pops the most recent isolated layer.
  void popIsolatedLayer() override;

  /**
   * Pushes a filter layer.
   *
   * @param effects The filter chain to apply when the layer is popped.
   */
  void pushFilterLayer(const components::FilterGraph& filterGraph,
                       const std::optional<Boxd>& filterRegion) override;

  /// Pops the most recent filter layer.
  void popFilterLayer() override;

  /**
   * Begins mask rendering.
   *
   * @param maskBounds Optional mask bounds clip.
   */
  void pushMask(const std::optional<Boxd>& maskBounds) override;

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
  void beginPatternTile(const Boxd& tileRect, const Transformd& targetFromPattern) override;

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
  void drawRect(const Boxd& rect, const StrokeParams& stroke) override;

  /**
   * Draws an ellipse.
   *
   * @param bounds The ellipse bounds.
   * @param stroke Stroke configuration for the ellipse.
   */
  void drawEllipse(const Boxd& bounds, const StrokeParams& stroke) override;

  /**
   * Draws an image resource.
   *
   * @param image The image resource to draw.
   * @param params Image placement parameters.
   */
  void drawImage(const ImageResource& image, const ImageParams& params) override;

  /**
   * Draws shaped text.
   *
   * @param text The shaped text runs.
   * @param params Text styling parameters.
   */
  void drawText(const components::ComputedTextComponent& text, const TextParams& params) override;

  /**
   * Captures a CPU-readable snapshot of the current frame.
   *
   * @return A snapshot of the rendered frame.
   */
  [[nodiscard]] RendererBitmap takeSnapshot() const override;

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
  [[nodiscard]] int width() const;

  /**
   * Returns the rendered height in pixels.
   *
   * @return The rendered height.
   */
  [[nodiscard]] int height() const;

private:
  std::unique_ptr<RendererImplementation> impl_;
};

}  // namespace donner::svg
