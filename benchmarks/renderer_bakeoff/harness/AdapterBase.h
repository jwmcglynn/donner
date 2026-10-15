#pragma once
/// @file
/// Common `RendererInterface` plumbing for third-party backend adapters: the transform stack,
/// paint resolution, clip classification, feature accounting and balanced push/pop bookkeeping.
/// Subclasses implement only the backend-specific draw and composition hooks.

#include <optional>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/FillRule.h"
#include "donner/base/Path.h"
#include "donner/base/Transform.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "harness/FeatureSupport.h"
#include "harness/PaintResolver.h"

namespace donner::bakeoff {

/// One clip-path shape with the transform that maps it into device space.
struct ClipShape {
  Path path;                              //!< Clip geometry in its own coordinate space.
  FillRule fillRule = FillRule::NonZero;  //!< Fill rule for this shape.
  Transform2d deviceFromPath;             //!< Maps \ref path into device pixels.
  /// Clip layer: shapes on the same layer are unioned, and layers intersect. Single-level clips
  /// put every shape on one layer.
  int layer = 0;
};

/// A clip the adapter must apply: the intersection of an optional rectangle with the union of
/// \ref shapes (when non-empty).
struct ClipDesc {
  std::optional<Box2d> rect;   //!< Clip rectangle in local space, if any.
  Transform2d deviceFromRect;  //!< Maps \ref rect into device pixels.
  /// Clip shapes; empty means no path clip. Shapes on one layer union, layers intersect.
  std::vector<ClipShape> shapes;
};

/**
 * Base class for bakeoff adapters. It drives Donner's `RendererDriver`, so every adapter receives
 * exactly the same traversal, and it routes each operation to a backend hook only when the
 * adapter's \ref CapabilityTable says the feature can be rendered. Operations the adapter cannot
 * render are dropped and counted in \ref usage, never handed to another renderer.
 */
class AdapterBase : public svg::RendererInterface {
public:
  /// @param capabilities Support level of each feature for this adapter.
  explicit AdapterBase(CapabilityTable capabilities);
  ~AdapterBase() override;

  /// Capabilities this adapter was constructed with.
  [[nodiscard]] const CapabilityTable& capabilities() const { return capabilities_; }

  /// Feature uses and drops accumulated since the last \ref resetUsage.
  [[nodiscard]] const FeatureUsage& usage() const { return usage_; }

  /// Clears \ref usage.
  void resetUsage() { usage_.reset(); }

  void draw(svg::SVGDocument& document) override;
  [[nodiscard]] int width() const override { return pixelWidth_; }
  [[nodiscard]] int height() const override { return pixelHeight_; }

  void beginFrame(const svg::RenderViewport& viewport) override;
  void endFrame() override;

  void setTransform(const Transform2d& deviceFromLocal) override;
  void pushTransform(const Transform2d& parentFromChild) override;
  void popTransform() override;

  void pushClip(const svg::ResolvedClip& clip) override;
  void popClip() override;

  void pushIsolatedLayer(double opacity, svg::MixBlendMode blendMode) override;
  void popIsolatedLayer() override;

  void pushFilterLayer(const svg::components::FilterGraph& filterGraph,
                       const std::optional<Box2d>& filterRegion) override;
  void popFilterLayer() override;

  void pushMask(const std::optional<Box2d>& maskBounds) override;
  void pushMask(const std::optional<Box2d>& maskBounds, svg::MaskType maskType) override;
  void pushMask(const std::optional<Box2d>& maskBounds, svg::MaskType maskType,
                svg::ColorInterpolation colorInterpolation) override;
  void transitionMaskToContent() override;
  void popMask() override;

  [[nodiscard]] bool beginPatternTile(const Box2d& tileRect,
                                      const Transform2d& targetFromPattern) override;
  void endPatternTile(bool forStroke) override;

  void setPaint(const svg::PaintParams& paint) override;
  void drawPath(const svg::PathShape& path, const svg::StrokeParams& stroke) override;
  void drawRect(const Box2d& rect, const svg::StrokeParams& stroke) override;
  void drawEllipse(const Box2d& bounds, const svg::StrokeParams& stroke) override;
  void drawImage(const svg::ImageResource& image, const svg::ImageParams& params) override;
  void drawText(Registry& registry, const svg::components::ComputedTextComponent& text,
                const svg::TextParams& params) override;

  [[nodiscard]] svg::RendererBitmap takeSnapshot() const override { return onSnapshot(); }

protected:
  /// Starts a frame of \p pixelWidth x \p pixelHeight device pixels, cleared to transparent.
  virtual void onBeginFrame(int pixelWidth, int pixelHeight) = 0;
  /// Finishes the frame and submits any recorded work.
  virtual void onEndFrame() = 0;
  /// Fills \p path. @param path Local-space geometry. @param fillRule Fill rule.
  /// @param paint Resolved paint. @param deviceFromLocal Local-to-device transform.
  virtual void onFillPath(const Path& path, FillRule fillRule, const PaintDesc& paint,
                          const Transform2d& deviceFromLocal) = 0;
  /// Strokes \p path. @param path Local-space geometry. @param stroke Stroke parameters.
  /// @param paint Resolved paint. @param deviceFromLocal Local-to-device transform.
  virtual void onStrokePath(const Path& path, const svg::StrokeParams& stroke,
                            const PaintDesc& paint, const Transform2d& deviceFromLocal) = 0;
  /// Reads the frame back as straight-alpha RGBA.
  virtual svg::RendererBitmap onSnapshot() const = 0;

  /// Pushes a clip. Called only when every clip feature in use is supported.
  /// @param clip Clip to apply.
  /// @return False when the adapter cannot express this particular clip; it is then dropped.
  virtual bool onPushClip(const ClipDesc& clip);
  /// Pops the clip pushed by the matching \ref onPushClip.
  virtual void onPopClip();
  /// Pushes an isolated group. @param opacity Group opacity. @param blendMode Blend mode.
  /// @return False when the adapter cannot express this group (for example its blend mode); the
  ///   group is then dropped.
  virtual bool onPushLayer(double opacity, svg::MixBlendMode blendMode);
  /// Pops the group pushed by the matching \ref onPushLayer.
  virtual void onPopLayer();
  /// Starts recording mask content. @param maskType Luminance or alpha.
  /// @param deviceFromLocal Transform at the push. @param bounds Optional local mask bounds.
  /// @return False when the adapter cannot express this mask; it is then dropped.
  virtual bool onPushMask(svg::MaskType maskType, const Transform2d& deviceFromLocal,
                          const std::optional<Box2d>& bounds);
  /// Switches from mask content to masked content.
  virtual void onMaskContent();
  /// Composites the masked content.
  virtual void onPopMask();
  /// Pushes a filter layer when the adapter can express \p graph.
  /// @param graph Filter graph. @param deviceFromLocal Transform at the push.
  /// @return True when the adapter applies the filter; false drops it.
  virtual bool onPushFilter(const svg::components::FilterGraph& graph,
                            const Transform2d& deviceFromLocal);
  /// Pops a filter accepted by \ref onPushFilter.
  virtual void onPopFilter();
  /// Draws a raster image. @param image Straight-alpha RGBA pixels.
  /// @param targetRect Local destination rectangle. @param deviceFromLocal Local-to-device.
  /// @param pixelated Nearest-neighbor sampling requested.
  /// @return False when the adapter did not draw the image; it is then counted as dropped.
  virtual bool onDrawImage(const svg::ImageResource& image, const Box2d& targetRect,
                           const Transform2d& deviceFromLocal, bool pixelated);
  /// Draws text. No adapter routes Donner's laid-out text yet, so the default draws nothing.
  /// @param registry Document registry. @param text Laid-out text. @param params Text paint.
  /// @return False when the text was not drawn; it is then counted as dropped.
  virtual bool onDrawText(Registry& registry, const svg::components::ComputedTextComponent& text,
                          const svg::TextParams& params);

  /// Starts recording a pattern tile. @param tileRect Tile rectangle in pattern space.
  /// @param targetFromPattern Pattern-to-target transform. @return False rejects the tile (it is
  ///   counted as dropped). An adapter that accepts a tile receives the next fill or stroke it
  ///   paints with \ref PaintDesc::Kind::Pattern and must render it.
  virtual bool onBeginPatternTile(const Box2d& tileRect, const Transform2d& targetFromPattern);
  /// Ends a tile accepted by \ref onBeginPatternTile. @param forStroke Tile paints the stroke.
  virtual void onEndPatternTile(bool forStroke);

  /// Current local-to-device transform.
  [[nodiscard]] const Transform2d& deviceFromLocal() const { return deviceFromLocal_; }

private:
  enum class PushKind : uint8_t { Clip, Layer, Filter, Mask };
  struct PushRecord {
    PushKind kind;
    bool forwarded;  //!< True when the matching backend hook ran.
  };

  /// Records a use of \p feature and returns whether the adapter renders it.
  bool admit(Feature feature);
  /// \ref admit for every feature; true only when all are rendered.
  bool admitAll(const std::vector<Feature>& features);
  /// Admits the paint family of \p paint.
  bool admitPaint(const PaintDesc& paint);
  void drawFill(const Path& path, FillRule fillRule, const Box2d& bounds);
  void drawStroke(const Path& path, const svg::StrokeParams& stroke, const Box2d& bounds);
  /// True while unsupported mask content is being skipped.
  [[nodiscard]] bool suppressed() const { return suppressDepth_ > 0; }
  void drawShape(const Path& path, FillRule fillRule, const svg::StrokeParams& stroke);
  void popRecord(PushKind kind);

  CapabilityTable capabilities_;
  FeatureUsage usage_;
  int pixelWidth_ = 0;
  int pixelHeight_ = 0;
  Transform2d deviceFromLocal_;
  std::vector<Transform2d> transformStack_;
  std::vector<PushRecord> pushStack_;
  /// Depth of unsupported masks whose definition content is currently being skipped.
  int suppressDepth_ = 0;
  /// Mask records whose content phase must end suppression, parallel to mask pushes.
  std::vector<bool> maskSuppressing_;
  svg::PaintParams paint_;
  /// Pattern paints recorded at one pattern-tile nesting depth. A tile ended at a depth becomes
  /// pending there; the next setPaint at that depth makes it the active paint for that element.
  struct PatternState {
    bool pendingFill = false;
    bool pendingStroke = false;
    bool activeFill = false;
    bool activeStroke = false;
  };
  /// Pattern state per tile depth; index 0 is the frame itself.
  std::vector<PatternState> patternStates_ = std::vector<PatternState>(1);
  /// Number of accepted pattern tiles currently being recorded.
  std::size_t tileDepth_ = 0;
};

}  // namespace donner::bakeoff
