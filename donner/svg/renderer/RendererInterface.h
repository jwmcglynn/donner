#pragma once
/// @file

#include <cstddef>
#include <optional>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/RelativeLengthMetrics.h"
#include "donner/base/RcString.h"
#include "donner/base/SmallVector.h"
#include "donner/base/Transform.h"
#include "donner/css/Color.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/core/FillRule.h"
#include "donner/svg/core/PathSpline.h"
#include "donner/svg/core/Stroke.h"
#include "donner/svg/resources/ImageResource.h"

namespace donner::svg {

/**
 * Describes the viewport for a render pass.
 */
struct RenderViewport {
  /// Logical size in CSS/SVG units after layout.
  Vector2d size = Vector2d::Zero();
  /// Device pixel ratio used to map logical coordinates to device pixels.
  double devicePixelRatio = 1.0;
};

/**
 * CPU-readable bitmap produced by a renderer snapshot.
 */
struct RendererBitmap {
  /// Pixel dimensions of the bitmap in device pixels.
  Vector2i dimensions = Vector2i::Zero();
  /// Raw pixel data in tightly packed RGBA 8-bit format.
  std::vector<uint8_t> pixels;
  /// Bytes between rows; allows alignment/padding differences between renderers.
  std::size_t rowBytes = 0;

  [[nodiscard]] bool empty() const {
    return dimensions.x <= 0 || dimensions.y <= 0 || pixels.empty();
  }
};

/**
 * Represents a resolved path along with its fill rule.
 */
struct PathShape {
  PathSpline path;
  FillRule fillRule = FillRule::NonZero;
};

/**
 * Stroke configuration used for path and primitive drawing.
 */
struct StrokeParams {
  /// Stroke width in user units.
  double strokeWidth = 0.0;
  StrokeLinecap lineCap = StrokeLinecap::Butt;
  StrokeLinejoin lineJoin = StrokeLinejoin::Miter;
  /// Maximum miter ratio before converting to bevel.
  double miterLimit = 4.0;
  /// Dash pattern lengths alternating on/off segments.
  std::vector<double> dashArray;
  /// Dash phase offset.
  double dashOffset = 0.0;
};

/**
 * Paint state derived from resolved style for the current node.
 */
struct PaintParams {
  /// Multiplicative opacity applied to all drawing.
  double opacity = 1.0;
  components::ResolvedPaintServer fill = PaintServer::None{};
  components::ResolvedPaintServer stroke = PaintServer::None{};
  double fillOpacity = 1.0;
  double strokeOpacity = 1.0;
  /// CurrentColor value for paint server resolution.
  css::Color currentColor = css::Color(css::RGBA());
  /// View box used for unit resolution and gradient coordinate conversion.
  Boxd viewBox;
  StrokeParams strokeParams;
};

/**
 * Clip stack entry combining rectangles, paths, and optional masks.
 */
struct ResolvedClip {
  std::optional<Boxd> clipRect;
  std::vector<PathShape> clipPaths;
  std::optional<components::ResolvedMask> mask;

  [[nodiscard]] bool empty() const { return !clipRect.has_value() && clipPaths.empty() && !mask; }
};

/**
 * Parameters describing how an image should be drawn.
 */
struct ImageParams {
  /// Destination rectangle in device-independent units.
  Boxd targetRect;
  double opacity = 1.0;
  /// Whether to favor nearest-neighbor sampling for pixelated rendering.
  bool imageRenderingPixelated = false;
};

/**
 * Parameters describing how text is drawn and outlined.
 */
struct TextParams {
  double opacity = 1.0;
  css::Color fillColor = css::Color(css::RGBA());
  css::Color strokeColor = css::Color(css::RGBA());
  StrokeParams strokeParams;
  SmallVector<RcString, 1> fontFamilies;
  Lengthd fontSize;
  Boxd viewBox;
  FontMetrics fontMetrics;
};

/**
 * Backend-agnostic rendering interface consumed by the traversal driver.
 */
class RendererInterface {
 public:
  virtual ~RendererInterface() = default;

  /**
   * Begins a render pass with the given viewport. Implementations may allocate or reset
   * backend-specific frame resources here.
   */
  virtual void beginFrame(const RenderViewport& viewport) = 0;

  /**
   * Completes the current render pass, flushing any pending work.
   */
  virtual void endFrame() = 0;

  /**
   * Pushes a transform onto the renderer stack, composing with the current transform.
   */
  virtual void pushTransform(const Transformd& transform) = 0;

  /**
   * Pops the most recent transform from the renderer stack.
   */
  virtual void popTransform() = 0;

  /**
   * Pushes a clip path/mask onto the renderer stack.
   */
  virtual void pushClip(const ResolvedClip& clip) = 0;

  /**
   * Pops the most recent clip from the renderer stack.
   */
  virtual void popClip() = 0;

  /**
   * Sets the active paint parameters used by subsequent draw calls.
   */
  virtual void setPaint(const PaintParams& paint) = 0;

  /**
   * Draws an arbitrary path using the current paint state.
   */
  virtual void drawPath(const PathShape& path, const StrokeParams& stroke) = 0;

  /**
   * Convenience helper for drawing axis-aligned rectangles.
   */
  virtual void drawRect(const Boxd& rect, const StrokeParams& stroke) = 0;

  /**
   * Convenience helper for drawing ellipses bounded by the provided box.
   */
  virtual void drawEllipse(const Boxd& bounds, const StrokeParams& stroke) = 0;

  /**
   * Draws an image resource into the given target rectangle.
   */
  virtual void drawImage(const ImageResource& image, const ImageParams& params) = 0;

  /**
   * Draws pre-shaped text with the provided paint parameters.
   */
  virtual void drawText(const components::ComputedTextComponent& text,
                        const TextParams& params) = 0;

  /**
   * Captures a CPU-readable snapshot of the current frame buffer for testing or downstream
   * consumers. The snapshot must remain valid after the render pass completes.
   */
  [[nodiscard]] virtual RendererBitmap takeSnapshot() const = 0;
};

}  // namespace donner::svg
