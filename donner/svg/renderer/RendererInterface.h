#pragma once
/// @file

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/RcString.h"
#include "donner/base/RelativeLengthMetrics.h"
#include "donner/base/SmallVector.h"
#include "donner/base/Transform.h"
#include "donner/css/Color.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/filter/FilterGraph.h"
#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/core/DominantBaseline.h"
#include "donner/svg/core/FillRule.h"
#include "donner/svg/core/LengthAdjust.h"
#include "donner/svg/core/PathSpline.h"
#include "donner/svg/core/Stroke.h"
#include "donner/svg/core/TextAnchor.h"
#include "donner/svg/core/TextDecoration.h"
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
 * Represents a resolved path along with its fill rule, transform, and layer index for boolean ops.
 */
struct PathShape {
  PathSpline path;
  FillRule fillRule = FillRule::NonZero;
  /// Transform from clip path child to the clip path's coordinate system.
  Transformd entityFromParent;
  /// Layer index for boolean combination: paths on the same layer are unioned, layers are
  /// intersected.
  int layer = 0;
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
  /// SVG pathLength attribute value; 0 means unused. When non-zero, dash arrays and offsets are
  /// scaled by the ratio of the actual path length to this value.
  double pathLength = 0.0;
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
  /// Transform applied to all clip paths (e.g., objectBoundingBox unit mapping).
  Transformd clipPathUnitsTransform;
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
  TextAnchor textAnchor = TextAnchor::Start;
  TextDecoration textDecoration = TextDecoration::None;
  DominantBaseline dominantBaseline = DominantBaseline::Auto;
  /// If set, stretches or compresses text to fill the given length.
  std::optional<Lengthd> textLength;
  LengthAdjust lengthAdjust = LengthAdjust::Default;
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
   * Sets the absolute transform on the renderer, replacing the current matrix.
   * Unlike pushTransform, this does not interact with the save/restore stack.
   */
  virtual void setTransform(const Transformd& transform) = 0;

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
   * Pushes an isolated compositing layer with the given opacity. Content drawn between
   * pushIsolatedLayer and popIsolatedLayer is composited as a group at the specified opacity.
   */
  virtual void pushIsolatedLayer(double opacity) = 0;

  /**
   * Pops the most recent isolated layer, compositing it with the given opacity.
   */
  virtual void popIsolatedLayer() = 0;

  /**
   * Pushes a filter layer that applies the given filter graph to all content drawn within it.
   *
   * @param filterGraph The filter graph describing primitives and their connections.
   * @param filterRegion The filter region bounds in local coordinates, used to clip the filter
   *   output. If nullopt, the filter operates on the full surface.
   */
  virtual void pushFilterLayer(const components::FilterGraph& filterGraph,
                               const std::optional<Boxd>& filterRegion) = 0;

  /**
   * Pops the most recent filter layer.
   */
  virtual void popFilterLayer() = 0;

  /**
   * Begins mask rendering. The driver renders the mask content between pushMask and
   * transitionMaskToContent, then renders the actual content between transitionMaskToContent
   * and popMask.
   *
   * @param maskBounds Optional clip rect for the mask region.
   */
  virtual void pushMask(const std::optional<Boxd>& maskBounds) = 0;

  /**
   * Transitions from rendering mask content to rendering masked content.
   * Must be called between pushMask and popMask.
   */
  virtual void transitionMaskToContent() = 0;

  /**
   * Pops the mask layer stack, compositing the masked content.
   */
  virtual void popMask() = 0;

  /**
   * Begins recording content into a pattern tile. Content drawn between beginPatternTile and
   * endPatternTile is captured as a repeating pattern.
   *
   * @param tileRect The tile rectangle in pattern coordinate space.
   * @param targetFromPattern Transform from pattern tile space to target element space.
   */
  virtual void beginPatternTile(const Boxd& tileRect, const Transformd& targetFromPattern) = 0;

  /**
   * Ends pattern tile recording and sets the resulting tiled shader as the current fill or stroke
   * paint.
   *
   * @param forStroke If true, set the pattern as the stroke paint; otherwise as the fill paint.
   */
  virtual void endPatternTile(bool forStroke) = 0;

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
