#pragma once
/// @file
///
/// `SerializingRenderer` is a `RendererInterface` implementation that encodes
/// each virtual method call to a `WireWriter` instead of rasterizing. It runs
/// the normal `RendererDriver` inside its `draw()` entry point, so every
/// call the driver would have made into a real backend (transform stack, paint
/// state, draw primitives) ends up as a wire message.
///
/// Supported opcodes (S2 scope):
///  - frame lifecycle, transforms, isolated layers
///  - solid-color paint (`PaintServer::None` / `PaintServer::Solid` only)
///  - clip state (rect + path shapes, no masks)
///  - drawPath, drawRect, drawEllipse
///
/// Anything else emits a `kUnsupported` message and increments
/// `unsupportedCount()` — the driver keeps making calls against us but the
/// resulting stream is flagged lossy.

#include <cstddef>
#include <cstdint>
#include <memory>

#include "donner/editor/sandbox/Wire.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::editor::sandbox {

class SerializingRenderer final : public svg::RendererInterface {
public:
  SerializingRenderer();
  ~SerializingRenderer() override;

  SerializingRenderer(const SerializingRenderer&) = delete;
  SerializingRenderer& operator=(const SerializingRenderer&) = delete;

  /// @name RendererInterface
  /// @{
  void draw(svg::SVGDocument& document) override;
  [[nodiscard]] int width() const override { return width_; }
  [[nodiscard]] int height() const override { return height_; }

  void beginFrame(const svg::RenderViewport& viewport) override;
  void endFrame() override;

  void setTransform(const Transform2d& transform) override;
  void pushTransform(const Transform2d& transform) override;
  void popTransform() override;

  void pushClip(const svg::ResolvedClip& clip) override;
  void popClip() override;

  void pushIsolatedLayer(double opacity, svg::MixBlendMode blendMode) override;
  void popIsolatedLayer() override;

  void pushFilterLayer(const svg::components::FilterGraph& filterGraph,
                       const std::optional<Box2d>& filterRegion) override;
  void popFilterLayer() override;

  void pushMask(const std::optional<Box2d>& maskBounds) override;
  void transitionMaskToContent() override;
  void popMask() override;

  void beginPatternTile(const Box2d& tileRect, const Transform2d& targetFromPattern) override;
  void endPatternTile(bool forStroke) override;

  void setPaint(const svg::PaintParams& paint) override;

  void drawPath(const svg::PathShape& path, const svg::StrokeParams& stroke) override;
  void drawRect(const Box2d& rect, const svg::StrokeParams& stroke) override;
  void drawEllipse(const Box2d& bounds, const svg::StrokeParams& stroke) override;

  void drawImage(const svg::ImageResource& image, const svg::ImageParams& params) override;
  void drawText(Registry& registry, const svg::components::ComputedTextComponent& text,
                const svg::TextParams& params) override;

  [[nodiscard]] svg::RendererBitmap takeSnapshot() const override;
  [[nodiscard]] std::unique_ptr<svg::RendererInterface> createOffscreenInstance() const override;
  /// @}

  /// Number of `kUnsupported` messages emitted so far. A value of 0 means the
  /// stream is faithful to the original call sequence; any other value means
  /// the replay renderer will miss at least one draw call.
  [[nodiscard]] std::size_t unsupportedCount() const { return unsupportedCount_; }

  /// True iff `unsupportedCount() > 0`.
  [[nodiscard]] bool hasUnsupported() const { return unsupportedCount_ > 0; }

  /// Move the accumulated wire bytes out of the writer. Invalidates the
  /// renderer; call exactly once, after `draw()` returns.
  [[nodiscard]] std::vector<uint8_t> takeBuffer() && {
    return std::move(writer_).take();
  }

  /// View of the accumulated wire bytes without transferring ownership.
  [[nodiscard]] std::span<const uint8_t> data() const { return writer_.data(); }

private:
  void writeStreamHeader();
  void writeUnsupported(UnsupportedKind kind);

  WireWriter writer_;
  int width_ = 0;
  int height_ = 0;
  std::size_t unsupportedCount_ = 0;
  bool headerWritten_ = false;
};

}  // namespace donner::editor::sandbox
