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
#include "donner/css/FontFace.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/filter/FilterEffect.h"
#include "donner/svg/components/filter/FilterGraph.h"
#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/core/DominantBaseline.h"
#include "donner/svg/core/FillRule.h"
#include "donner/svg/core/LengthAdjust.h"
#include "donner/svg/core/MixBlendMode.h"
#include "donner/svg/core/PathSpline.h"
#include "donner/svg/core/TextAnchor.h"
#include "donner/svg/core/TextDecoration.h"
#include "donner/svg/core/WritingMode.h"
#include "donner/svg/renderer/StrokeParams.h"
#include "donner/svg/resources/ImageResource.h"

namespace donner::svg {

class SVGDocument;

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

  ResolvedClip() = default;
  ResolvedClip(ResolvedClip&&) = default;
  ResolvedClip& operator=(ResolvedClip&&) = default;

  /// Deep copy (clones mask chain).
  ResolvedClip(const ResolvedClip& other)
      : clipRect(other.clipRect),
        clipPaths(other.clipPaths),
        clipPathUnitsTransform(other.clipPathUnitsTransform),
        mask(other.mask ? std::optional<components::ResolvedMask>(other.mask->deepCopy())
                        : std::nullopt) {}

  ResolvedClip& operator=(const ResolvedClip& other) {
    if (this != &other) {
      clipRect = other.clipRect;
      clipPaths = other.clipPaths;
      clipPathUnitsTransform = other.clipPathUnitsTransform;
      mask = other.mask ? std::optional<components::ResolvedMask>(other.mask->deepCopy())
                        : std::nullopt;
    }
    return *this;
  }

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
  /// CSS `writing-mode` for this text element. Controls horizontal vs vertical text flow.
  WritingMode writingMode = WritingMode::HorizontalTb;
  /// Extra spacing added after each character (CSS `letter-spacing`). 0 = normal.
  double letterSpacingPx = 0.0;
  /// Extra spacing added after each word/space character (CSS `word-spacing`). 0 = normal.
  double wordSpacingPx = 0.0;
  /// If set, stretches or compresses text to fill the given length.
  std::optional<Lengthd> textLength;
  LengthAdjust lengthAdjust = LengthAdjust::Default;
  /// `@font-face` declarations for custom font resolution.
  std::span<const css::FontFace> fontFaces;
  /// Entity of the text root element, for cached layout lookup. entt::null if unknown.
  entt::entity textRootEntity = entt::null;
};

/**
 * Backend-agnostic rendering interface consumed by RendererDriver during document traversal.
 *
 * Implement this interface to create a custom rendering backend. The driver calls methods in the
 * following order:
 *
 * 1. **Frame lifecycle:** `draw()` → `beginFrame()` → (drawing calls) → `endFrame()`
 * 2. **State stacks:** `pushTransform()`/`popTransform()`, `pushClip()`/`popClip()`,
 *    `pushIsolatedLayer()`/`popIsolatedLayer()` are always paired (LIFO).
 * 3. **Paint:** `setPaint()` is called before each draw operation.
 * 4. **Masks:** `pushMask()` → (render mask shape) → `transitionMaskToContent()` →
 *    (render masked content) → `popMask()`. Masks may nest.
 *
 * Coordinates flow through three spaces: SVG user units → document coordinates (after viewBox
 * mapping) → device pixels (after `devicePixelRatio` scaling in \ref RenderViewport).
 */
class RendererInterface {
public:
  virtual ~RendererInterface() = default;

  /**
   * Renders the given SVG document. Implementations prepare the document, traverse the render
   * tree, and emit drawing commands.
   */
  virtual void draw(SVGDocument& document) = 0;

  /// Returns the rendered width in device pixels.
  [[nodiscard]] virtual int width() const = 0;

  /// Returns the rendered height in device pixels.
  [[nodiscard]] virtual int height() const = 0;

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
   * Pushes an isolated compositing layer with the given opacity and blend mode. Content drawn
   * between pushIsolatedLayer and popIsolatedLayer is composited as a group at the specified
   * opacity using the specified blend mode.
   */
  virtual void pushIsolatedLayer(double opacity, MixBlendMode blendMode) = 0;

  /**
   * Pops the most recent isolated layer, compositing it with the given opacity.
   */
  virtual void popIsolatedLayer() = 0;

  /**
   * Pushes a filter layer that applies the given effect chain to all content drawn within it.
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
   * @param patternToTarget Transform from pattern tile space to target element space.
   */
  virtual void beginPatternTile(const Boxd& tileRect, const Transformd& patternToTarget) = 0;

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
  virtual void drawText(Registry& registry, const components::ComputedTextComponent& text,
                        const TextParams& params) = 0;

  /**
   * Captures a CPU-readable snapshot of the current frame buffer for testing or downstream
   * consumers. The snapshot must remain valid after the render pass completes.
   */
  [[nodiscard]] virtual RendererBitmap takeSnapshot() const = 0;

  /**
   * Creates an independent offscreen renderer instance of the same type as this one.
   * Used for rendering sub-documents into intermediate pixmaps when a backend needs an isolated
   * offscreen pass.
   * Returns nullptr if offscreen rendering is not supported.
   */
  [[nodiscard]] virtual std::unique_ptr<RendererInterface> createOffscreenInstance() const {
    return nullptr;
  }
};

}  // namespace donner::svg
