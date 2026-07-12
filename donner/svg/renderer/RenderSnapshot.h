#pragma once
/// @file

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "donner/svg/renderer/RendererInterface.h"

namespace donner::svg {

/**
 * Immutable command stream captured from a prepared SVG document.
 *
 * Snapshot capture may inspect the live ECS registry, but replay emits only the
 * recorded renderer commands. Resource references embedded in those commands
 * point at snapshot-owned storage so backend replay does not dereference the
 * live document registry.
 */
class RenderSnapshot {
public:
  /// Create an empty render snapshot.
  RenderSnapshot();

  /// Destructor.
  ~RenderSnapshot();

  /// Move constructor.
  RenderSnapshot(RenderSnapshot&& other) noexcept;

  /// Move assignment.
  RenderSnapshot& operator=(RenderSnapshot&& other) noexcept;

  RenderSnapshot(const RenderSnapshot& other) = delete;
  RenderSnapshot& operator=(const RenderSnapshot& other) = delete;

  /// Source document revision observed when this snapshot was captured.
  [[nodiscard]] std::uint64_t sourceRevision() const;

  /// Number of renderer commands stored in this snapshot.
  [[nodiscard]] std::size_t commandCount() const;

  /**
   * Estimated bytes used by the snapshot command stream.
   *
   * This counts the command vector allocation and snapshot implementation
   * object. It intentionally does not include nested allocations owned by
   * command payloads, such as paths, text spans, filter graphs, or paint-server
   * resource components.
   */
  [[nodiscard]] std::size_t estimatedCommandStorageBytes() const;

  /**
   * Count snapshot-owned command references that still point at \p registry.
   *
   * This is a test-only invariant helper: a captured snapshot should not retain
   * any \ref EntityHandle into the live document registry.
   *
   * @param registry Registry that must not be referenced by replay payloads.
   */
  [[nodiscard]] std::size_t liveRegistryReferenceCountForTesting(const Registry& registry) const;

  /**
   * Replay the captured command stream into \p renderer.
   *
   * @param renderer Backend receiving the recorded commands.
   */
  void replay(RendererInterface& renderer) const;

private:
  friend class RenderSnapshotRecorder;
  friend class RendererDriver;

  struct Impl;

  void setSourceRevision(std::uint64_t revision);

  std::unique_ptr<Impl> impl_;
};

/**
 * RendererInterface implementation that records commands into a
 * \ref RenderSnapshot.
 */
class RenderSnapshotRecorder final : public RendererInterface {
public:
  /**
   * Create a recorder.
   *
   * @param snapshot Snapshot receiving recorded commands.
   * @param offscreenFactory Backend used only for creating offscreen renderers
   *     while capture prepares filters, masks, and sub-documents.
   */
  RenderSnapshotRecorder(RenderSnapshot& snapshot, RendererInterface& offscreenFactory);

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
  [[nodiscard]] bool beginPatternTile(const Box2d& tileRect,
                                      const Transform2d& targetFromPattern) override;
  void endPatternTile(bool forStroke) override;
  void setPaint(const PaintParams& paint) override;
  void drawPath(const PathShape& path, const StrokeParams& stroke) override;
  void drawRect(const Box2d& rect, const StrokeParams& stroke) override;
  void drawEllipse(const Box2d& bounds, const StrokeParams& stroke) override;
  void drawImage(const ImageResource& image, const ImageParams& params) override;
  void drawText(Registry& registry, const components::ComputedTextComponent& text,
                const TextParams& params) override;
  [[nodiscard]] RendererBitmap takeSnapshot() const override;
  [[nodiscard]] std::unique_ptr<RendererInterface> createOffscreenInstance() const override;

private:
  RenderSnapshot& snapshot_;
  RendererInterface& offscreenFactory_;
};

}  // namespace donner::svg
