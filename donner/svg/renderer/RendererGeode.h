#pragma once
/// @file
/// Geode (WebGPU/Slug) implementation of \ref donner::svg::RendererInterface.
///
/// This is the **skeleton** stage of the Geode backend: only solid-color path
/// fills round-trip through the GPU pipeline. Stroke, gradient, pattern,
/// image, text, clip, mask, layer, and filter operations are stubbed and will
/// land in later phases (see `docs/design_docs/geode_renderer.md`).

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/Transform.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::geode {
class GeodeDevice;
class GeodePipeline;
class GeoEncoder;
}  // namespace donner::geode

namespace donner::svg {

/**
 * Geode rendering backend — GPU-native via WebGPU + the Slug algorithm.
 *
 * `RendererGeode` implements `RendererInterface` by translating draw calls
 * into the lower-level `donner::geode::GeoEncoder` API. It owns its own
 * headless `GeodeDevice` and a single `GeodePipeline` (Slug fill).
 *
 * Currently supported (skeleton):
 * - `beginFrame` / `endFrame` lifecycle and `takeSnapshot` readback.
 * - Transform stack (`setTransform`, `pushTransform`, `popTransform`).
 * - Solid-color `drawPath` (any `Path`, both fill rules) — strokes ignored.
 * - `drawRect` and `drawEllipse` via path conversion.
 *
 * Stubbed (no-op, optionally warned in verbose mode):
 * - Clips, masks, isolated layers, filter layers, pattern tiles.
 * - Gradient and pattern paint servers (only `PaintServer::Solid` is honored).
 * - `drawImage`, `drawText`.
 *
 * If `GeodeDevice::CreateHeadless()` fails (no GPU available), all draw
 * operations become no-ops and `takeSnapshot()` returns an empty bitmap.
 */
class RendererGeode : public RendererInterface {
public:
  /**
   * Construct the renderer. Creates a headless `GeodeDevice` immediately;
   * if device creation fails, the renderer enters a "no-op" state and all
   * subsequent draw calls do nothing.
   *
   * @param verbose If true, emit warnings to stderr for unsupported features
   *   the first time they are encountered.
   */
  explicit RendererGeode(bool verbose = false);

  ~RendererGeode() override;

  RendererGeode(const RendererGeode&) = delete;
  RendererGeode& operator=(const RendererGeode&) = delete;
  /// Move constructor.
  RendererGeode(RendererGeode&&) noexcept;
  /// Move assignment operator.
  RendererGeode& operator=(RendererGeode&&) noexcept;

  // --- RendererInterface ---

  void draw(SVGDocument& document) override;

  [[nodiscard]] int width() const override;
  [[nodiscard]] int height() const override;

  void beginFrame(const RenderViewport& viewport) override;
  void endFrame() override;

  void setTransform(const Transform2d& transform) override;
  void pushTransform(const Transform2d& transform) override;
  void popTransform() override;

  void pushClip(const ResolvedClip& clip) override;
  void popClip() override;

  void pushIsolatedLayer(double opacity, MixBlendMode blendMode) override;
  void popIsolatedLayer() override;

  void pushFilterLayer(const components::FilterGraph& filterGraph,
                       const std::optional<Box2d>& filterRegion) override;
  void popFilterLayer() override;

  void pushMask(const std::optional<Box2d>& maskBounds) override;
  void transitionMaskToContent() override;
  void popMask() override;

  void beginPatternTile(const Box2d& tileRect, const Transform2d& targetFromPattern) override;
  void endPatternTile(bool forStroke) override;

  void setPaint(const PaintParams& paint) override;

  void drawPath(const PathShape& path, const StrokeParams& stroke) override;
  void drawRect(const Box2d& rect, const StrokeParams& stroke) override;
  void drawEllipse(const Box2d& bounds, const StrokeParams& stroke) override;

  void drawImage(const ImageResource& image, const ImageParams& params) override;
  void drawText(Registry& registry, const components::ComputedTextComponent& text,
                const TextParams& params) override;

  [[nodiscard]] RendererBitmap takeSnapshot() const override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace donner::svg
