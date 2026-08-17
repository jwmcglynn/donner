#pragma once
/// @file

#include <cstdint>
#include <memory>
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
   * @return True when recording started. False rejects an unsafe tile before allocation.
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
   * Zero-copy compose blit for premultiplied CPU bitmaps.
   *
   * tiny-skia consumes premultiplied RGBA natively, so a tightly-packed
   * premultiplied \ref RendererBitmap is drawn through a borrowed `PixmapView`
   * with no pixel-buffer conversion or copy. Padded and non-premultiplied
   * bitmaps are packed into draw scratch before creating the view.
   *
   * @param bitmap The bitmap to draw.
   * @param params Image placement parameters.
   */
  void drawBitmap(const RendererBitmap& bitmap, const ImageParams& params) override;

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
  [[nodiscard]] std::unique_ptr<RendererInterface> createOffscreenInstance() const override;

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
    Transform2d targetFromPattern;
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
    std::optional<tiny_skia::Pixmap> fillPaintPixmap;
    std::optional<tiny_skia::Pixmap> strokePaintPixmap;
    double opacity = 1.0;
    MixBlendMode blendMode = MixBlendMode::Normal;
    components::FilterGraph filterGraph;
    std::optional<Box2d> filterRegion;
    Transform2d deviceFromFilter;
    int filterBufferOffsetX = 0;
    int filterBufferOffsetY = 0;
    std::optional<Box2d> maskBounds;
    Transform2d maskBoundsTransform;
    std::optional<tiny_skia::Mask> maskAlpha;
    Transform2d targetFromPattern;
    Transform2d patternRasterFromTile;
    Transform2d savedTransform;
    std::vector<Transform2d> savedTransformStack;
    std::optional<tiny_skia::Mask> savedClipMask;
    std::vector<std::optional<tiny_skia::Mask>> savedClipStack;
    /// Pattern paints pending at `beginPatternTile` time, saved so tile-content draws don't
    /// consume the outer element's pattern shaders (e.g. a `context-fill` pattern shared between
    /// several consumers re-rendering the same tile subtree). Restored by `endPatternTile`.
    std::optional<PatternPaintState> savedPatternFillPaint;
    /// @see savedPatternFillPaint
    std::optional<PatternPaintState> savedPatternStrokePaint;
  };

  [[nodiscard]] tiny_skia::Pixmap& currentPixmap();
  [[nodiscard]] const tiny_skia::Pixmap& currentPixmap() const;
  [[nodiscard]] tiny_skia::MutablePixmapView currentPixmapView();
  [[nodiscard]] std::optional<tiny_skia::Mask> buildClipMask(const ResolvedClip& clip) const;
  [[nodiscard]] std::optional<tiny_skia::Paint> makeFillPaint(const Box2d& bounds);
  [[nodiscard]] std::optional<tiny_skia::Paint> makeStrokePaint(const Box2d& bounds,
                                                                const StrokeParams& stroke);
  [[nodiscard]] tiny_skia::Pixmap createTransparentPixmap(int width, int height) const;
  /**
   * Builds the base paint for a pixmap composite into \p destination.
   *
   * @param destination Surface the composite writes to.
   * @param quality Sampling filter for the source pixmap.
   */
  [[nodiscard]] tiny_skia::PixmapPaint makePixmapPaint(const tiny_skia::Pixmap& destination,
                                                       tiny_skia::FilterQuality quality) const;
  void compositePixmapInto(tiny_skia::Pixmap& destination, const tiny_skia::Pixmap& pixmap,
                           double opacity, MixBlendMode blendMode = MixBlendMode::Normal);
  void compositePixmap(const tiny_skia::Pixmap& pixmap, double opacity,
                       MixBlendMode blendMode = MixBlendMode::Normal);
  void maybeWarnUnsupportedText();

  bool verbose_ = false;
  bool antialias_ = true;
  bool warnedUnsupportedText_ = false;
  RenderViewport viewport_;
  PaintParams paint_;
  double paintOpacity_ = 1.0;

  /// Top-level frame buffer.
  ///
  /// Storage model: every pixmap this backend owns, `frame_` included, holds
  /// premultiplied RGBA8, matching tiny-skia's native format. No draw pays for
  /// a per-pixel unpremultiply on store, and no composite into `frame_` pays
  /// for a premultiply-blend-unpremultiply round trip. \ref takeSnapshot
  /// converts once, on the way out, and is the only place it happens. That
  /// is worth roughly a 1.9x median settled-frame speedup across the CPU
  /// benchmark scenes.
  ///
  /// The cost is precision at low alpha: premultiplying before the float-to-u8
  /// store quantizes RGB, so straight (17,17,17,6) stores as premultiplied
  /// (0,0,0,6) and unpremultiplying cannot recover the 17s. Heavily antialiased
  /// edges of dark shapes shift by a few units as a result, measured at no more
  /// than 4/255 once composited over an opaque background. A straight-alpha
  /// store cannot lose that, because it never multiplies before rounding.
  ///
  /// This backend used to ask tiny-skia to unpremultiply on store instead, and
  /// that stage lived only in the float raster pipeline, so it was quietly
  /// pinning root draws to that pipeline too. \ref makePixmapPaint keeps that
  /// pin for composites into this buffer, deliberately: dropping it as well let
  /// the 8-bit compose path land opaque pixels at alpha 250. Keeping it costs
  /// nothing measurable, so the speedup above comes from removing the
  /// conversion stages, not from the 8-bit pipeline.
  tiny_skia::Pixmap frame_;
  Transform2d deviceFromLocalTransform_;
  std::vector<Transform2d> deviceFromLocalTransformStack_;
  std::optional<tiny_skia::Mask> currentClipMask_;
  std::vector<std::optional<tiny_skia::Mask>> clipStack_;
  std::vector<SurfaceFrame> surfaceStack_;
  std::optional<PatternPaintState> patternFillPaint_;
  std::optional<PatternPaintState> patternStrokePaint_;

  /// Staging buffer for image and bitmap payloads that have to be row-packed or premultiplied
  /// before tiny-skia can view them. Held across draws so a repeated compose reuses one
  /// allocation; only live for the duration of a single draw call, which never nests.
  std::vector<uint8_t> pixelScratch_;
};

}  // namespace donner::svg
