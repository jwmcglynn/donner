#pragma once
/// @file

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "tiny_skia/Mask.h"
#include "tiny_skia/Paint.h"
#include "tiny_skia/Pixmap.h"

namespace donner::svg {

/**
 * Rendering backend using tiny-skia-cpp.
 *
 * This backend provides a lightweight software rasterizer implementation of
 * \ref RendererInterface. It currently targets shape, image, gradient, clip,
 * mask, pattern, and opacity-layer rendering. Text rendering is not yet
 * implemented.
 */
class RendererTinySkia : public RendererInterface {
public:
  /**
   * Creates the tiny-skia renderer.
   *
   * @param verbose If true, emit diagnostic logging for unsupported features.
   */
  explicit RendererTinySkia(bool verbose = false);

  /// Destructor.
  ~RendererTinySkia();

  /// Move constructor.
  RendererTinySkia(RendererTinySkia&&) noexcept;
  /// Move assignment operator.
  RendererTinySkia& operator=(RendererTinySkia&&) noexcept;
  RendererTinySkia(const RendererTinySkia&) = delete;
  RendererTinySkia& operator=(const RendererTinySkia&) = delete;

  /**
   * Draws the SVG document using the renderer.
   *
   * @param document The SVG document to render.
   */
  void draw(SVGDocument& document) override;

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
  void pushFilterLayer(std::span<const FilterEffect> effects) override;

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
   * @param patternToTarget Transform from pattern tile space to target space.
   */
  void beginPatternTile(const Boxd& tileRect, const Transformd& patternToTarget) override;

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
   * Text rendering is not yet implemented for this backend.
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

  /// Enables or disables anti-aliasing.
  void setAntialias(bool antialias) { antialias_ = antialias; }

  /// Returns the rendered width in pixels.
  int width() const override;

  /// Returns the rendered height in pixels.
  int height() const override;

private:
  struct PatternPaintState {
    tiny_skia::Pixmap pixmap;
    Transformd patternToTarget;
  };

  enum class SurfaceKind {
    IsolatedLayer,
    FilterLayer,
    MaskCapture,
    MaskContent,
    PatternTile,
  };

  struct SurfaceFrame {
    SurfaceKind kind;
    tiny_skia::Pixmap pixmap;
    double opacity = 1.0;
    std::vector<FilterEffect> effects;
    std::optional<Boxd> maskBounds;
    Transformd maskBoundsTransform;
    std::optional<tiny_skia::Mask> maskAlpha;
    Transformd patternToTarget;
    Transformd patternRasterFromTile;
    Transformd savedTransform;
    std::vector<Transformd> savedTransformStack;
    std::optional<tiny_skia::Mask> savedClipMask;
    std::vector<std::optional<tiny_skia::Mask>> savedClipStack;
  };

  [[nodiscard]] tiny_skia::Pixmap& currentPixmap();
  [[nodiscard]] const tiny_skia::Pixmap& currentPixmap() const;
  [[nodiscard]] tiny_skia::MutablePixmapView currentPixmapView();
  [[nodiscard]] std::optional<tiny_skia::Mask> buildClipMask(const ResolvedClip& clip) const;
  [[nodiscard]] std::optional<tiny_skia::Paint> makeFillPaint(const Boxd& bounds);
  [[nodiscard]] std::optional<tiny_skia::Paint> makeStrokePaint(const Boxd& bounds,
                                                                const StrokeParams& stroke);
  [[nodiscard]] tiny_skia::Pixmap createTransparentPixmap(int width, int height) const;
  void compositePixmap(const tiny_skia::Pixmap& pixmap, double opacity);
  void applyFilters(tiny_skia::Pixmap& pixmap, std::span<const FilterEffect> effects);
  void maybeWarnUnsupportedFilter(const FilterEffect& effect);
  void maybeWarnUnsupportedText();

  bool verbose_ = false;
  bool antialias_ = true;
  bool warnedUnsupportedText_ = false;
  bool warnedUnsupportedFilter_ = false;

  RenderViewport viewport_;
  PaintParams paint_;
  double paintOpacity_ = 1.0;

  tiny_skia::Pixmap frame_;
  Transformd currentTransform_;
  std::vector<Transformd> transformStack_;
  std::optional<tiny_skia::Mask> currentClipMask_;
  std::vector<std::optional<tiny_skia::Mask>> clipStack_;
  std::vector<SurfaceFrame> surfaceStack_;
  std::optional<PatternPaintState> patternFillPaint_;
  std::optional<PatternPaintState> patternStrokePaint_;
};

}  // namespace donner::svg
