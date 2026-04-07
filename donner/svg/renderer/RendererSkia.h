#pragma once
/// @file

#include <functional>
#include <string>

#include "donner/base/EcsRegistry.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkGraphics.h"
#include "include/core/SkPictureRecorder.h"
#include "include/core/SkSurface.h"

namespace donner::svg {

/**
 * Rendering backend using Skia, https://github.com/google/skia
 *
 * Skia is a 2D graphics library that powers Chrome, Firefox, Android, and many other projects, and
 * supports all functionality required to implement SVG (as many of these projects also support
 * SVG).
 *
 * Skia is used as the reference renderer while implementing Donner, but long-term Donner would like
 * to support other rendering backends, so dependencies on Skia should be kept to a minimum and
 * isolated to RendererSkia.
 *
 * This is a prototype-quality implementation, and is subject to refactoring in the future to
 * provide a cleaner API boundary between Donner and the rendering backend.
 */
class RendererSkia : public RendererInterface {
public:
  /**
   * Create the Skia renderer.
   *
   * @param verbose  If true, print verbose logging.
   */
  explicit RendererSkia(bool verbose = false);

  /// Destructor.
  ~RendererSkia();

  // Move-only, no copy.
  /// Move constructor.
  RendererSkia(RendererSkia&&) noexcept;
  /// Move assignment operator.
  RendererSkia& operator=(RendererSkia&&) noexcept;
  RendererSkia(const RendererSkia&) = delete;
  RendererSkia& operator=(const RendererSkia&) = delete;

  /**
   * Draw the SVG document using the renderer. Writes to an internal bitmap, which can be
   * retrieved using the bitmap() method.
   *
   * @param document The SVG document to render.
   */
  void draw(SVGDocument& document) override;

  /**
   * Begins a render pass for the given viewport.
   */
  void beginFrame(const RenderViewport& viewport) override;

  /**
   * Completes the current render pass, flushing any buffered work.
   */
  void endFrame() override;

  /**
   * Sets the absolute transform, replacing the current matrix.
   */
  void setTransform(const Transform2d& transform) override;

  /**
   * Pushes a transform onto the Skia canvas stack.
   */
  void pushTransform(const Transform2d& transform) override;

  /**
   * Pops the most recently applied transform.
   */
  void popTransform() override;

  /**
   * Pushes a clip rect or path mask onto the Skia canvas stack.
   */
  void pushClip(const ResolvedClip& clip) override;

  /**
   * Pops the most recently applied clip.
   */
  void popClip() override;

  /**
   * Pushes an isolated compositing layer with the given opacity.
   */
  void pushIsolatedLayer(double opacity, MixBlendMode blendMode) override;

  /**
   * Pops the most recent isolated layer.
   */
  void popIsolatedLayer() override;

  /**
   * Pushes a filter layer that applies the given effect chain.
   */
  void pushFilterLayer(const components::FilterGraph& filterGraph,
                       const std::optional<Box2d>& filterRegion) override;

  /**
   * Pops the most recent filter layer.
   */
  void popFilterLayer() override;

  void pushMask(const std::optional<Box2d>& maskBounds) override;
  void transitionMaskToContent() override;
  void popMask() override;

  void beginPatternTile(const Box2d& tileRect, const Transform2d& targetFromPattern) override;
  void endPatternTile(bool forStroke) override;

  /**
   * Sets the current paint state for subsequent draw calls.
   */
  void setPaint(const PaintParams& paint) override;

  /**
   * Draws a path with fill and stroke derived from the current paint.
   */
  void drawPath(const PathShape& path, const StrokeParams& stroke) override;

  /**
   * Draws a rectangle convenience primitive.
   */
  void drawRect(const Box2d& rect, const StrokeParams& stroke) override;

  /**
   * Draws an ellipse convenience primitive.
   */
  void drawEllipse(const Box2d& bounds, const StrokeParams& stroke) override;

  /**
   * Draws an image resource.
   */
  void drawImage(const ImageResource& image, const ImageParams& params) override;

  /**
   * Draws text runs.
   */
  void drawText(Registry& registry, const components::ComputedTextComponent& text,
                const TextParams& params) override;

  /**
   * Captures a CPU-readable snapshot of the last-rendered frame.
   */
  [[nodiscard]] RendererBitmap takeSnapshot() const override;
  [[nodiscard]] std::unique_ptr<RendererInterface> createOffscreenInstance() const override;

  /**
   * Draw the given \ref SVGDocument into a SkPicture, for offscreen rendering or debugging
   * purposes.
   *
   * @param document The SVG document to render.
   */
  sk_sp<SkPicture> drawIntoSkPicture(SVGDocument& document);

  /**
   * Save the rendered image to a PNG file.
   *
   * @param filename The filename to save the PNG to.
   * @return True if the save was successful.
   */
  bool save(const char* filename);

  /**
   * Get the pixel data of the rendered image.
   *
   * @return A span of the pixel data, in RGBA format of size `width() * height() * 4`.
   */
  std::span<const uint8_t> pixelData() const;

  /// Get the width of the rendered image in pixels.
  int width() const override { return bitmap_.width(); }

  /// Get the height of the rendered image in pixels.
  int height() const override { return bitmap_.height(); }

  /// Get the SkBitmap of the rendered image.
  const SkBitmap& bitmap() const { return bitmap_; }

  /**
   * Enable or disable antialiasing. On by default.
   *
   * @param antialias Whether to enable antialiasing.
   */
  void setAntialias(bool antialias) { antialias_ = antialias; }

private:
  /// Implementation class.
  class Impl;

  bool verbose_;  //!< If true, print verbose logging.

  sk_sp<class SkFontMgr> fontMgr_;  //!< Font manager, may be initialized with custom fonts.
  std::map<std::string, sk_sp<SkTypeface>> typefaces_;  //!< Cached typefaces by family name.

  SkBitmap bitmap_;                         //!< The bitmap to render into.
  std::unique_ptr<SkCanvas> bitmapCanvas_;  //!< Direct canvas from bitmap.
  SkCanvas* currentCanvas_ = nullptr;       //!< The current canvas.
  bool antialias_ = true;                   //!< Whether to antialias.
  RenderViewport viewport_;
  PaintParams paint_;
  double paintOpacity_ = 1.0;
  int transformDepth_ = 0;
  int clipDepth_ = 0;
  SkCanvas* externalCanvas_ = nullptr;

  // Pattern recording state, stacked for nested patterns.
  struct PatternState {
    std::unique_ptr<SkPictureRecorder> recorder;
    SkCanvas* savedCanvas = nullptr;
    Transform2d targetFromPattern;
  };
  std::vector<PatternState> patternStack_;
  std::optional<SkPaint> patternFillPaint_;
  std::optional<SkPaint> patternStrokePaint_;

  struct ClipStackEntry {
    ResolvedClip clip;
    SkMatrix matrix;
  };

  struct FilterLayerState {
    bool usesNativeSkiaFilter = false;
    sk_sp<SkSurface> surface;
    sk_sp<SkSurface> fillPaintSurface;
    sk_sp<SkSurface> strokePaintSurface;
    int filterBufferOffsetX = 0;
    int filterBufferOffsetY = 0;
    SkCanvas* parentCanvas = nullptr;
    components::FilterGraph filterGraph;
    std::optional<Box2d> filterRegion;
    Transform2d originalDeviceFromFilter;
    Transform2d deviceFromFilter;
  };

  struct MaskLayerState {
    sk_sp<SkSurface> maskSurface;
    sk_sp<SkSurface> contentSurface;
    SkCanvas* parentCanvas = nullptr;
    /// Luminance alpha mask: one byte per pixel, computed from the mask surface.
    std::vector<uint8_t> maskAlpha;
  };

  std::vector<ClipStackEntry> activeClips_;
  std::vector<FilterLayerState> filterLayerStack_;
  std::vector<MaskLayerState> maskLayerStack_;

  std::optional<SkPaint> makeFillPaint(const Box2d& bounds);
  std::optional<SkPaint> makeStrokePaint(const Box2d& bounds, const StrokeParams& stroke);
  FilterLayerState* currentFilterLayerState();
  void drawOnFilterInputSurface(const sk_sp<SkSurface>& surface,
                                const std::function<void(SkCanvas*)>& drawFn);
};

}  // namespace donner::svg
