#pragma once
/// @file
/// Geode (WebGPU/Slug) implementation of \ref donner::svg::RendererInterface.
///
/// Geode is a GPU-native SVG rendering backend using WebGPU and the Slug
/// algorithm for resolution-independent vector rasterization. It can run
/// **headless** (creating its own device) or **embedded** inside a host
/// application that provides an existing WebGPU device and render target.
///
/// See `docs/design_docs/0017-geode_renderer.md` for the full design.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>
#include <webgpu/webgpu.hpp>

#include "donner/base/Box.h"
#include "donner/base/Transform.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/geode/GeodeCounters.h"
#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"

namespace donner::geode {
class GeodeDevice;
class GeodePipeline;
class GeoEncoder;
struct GeodeEmbedConfig;
}  // namespace donner::geode

// Forward-declare std::shared_ptr specialization to avoid pulling <memory>
// into every includer - <memory> is already included above.

namespace donner::svg {

/**
 * WebGPU texture snapshot exported by \ref RendererGeode.
 *
 * The snapshot keeps the backing \ref geode::GeodeDevice and texture alive so
 * editor presentation code can sample the texture after the renderer has moved
 * on to a later frame.
 */
class RendererGeodeTextureSnapshot final : public RendererTextureSnapshot {
public:
  /**
   * Construct a Geode texture snapshot.
   *
   * @param device Shared Geode device that owns the WebGPU handle lifetime.
   * @param texture Resolved single-sample texture containing the rendered frame.
   * @param dimensions Texture dimensions in device pixels.
   * @param format Texture format.
   */
  RendererGeodeTextureSnapshot(std::shared_ptr<geode::GeodeDevice> device, wgpu::Texture texture,
                               Vector2i dimensions, wgpu::TextureFormat format);
  ~RendererGeodeTextureSnapshot() override;

  RendererGeodeTextureSnapshot(const RendererGeodeTextureSnapshot&) = delete;
  RendererGeodeTextureSnapshot& operator=(const RendererGeodeTextureSnapshot&) = delete;
  RendererGeodeTextureSnapshot(RendererGeodeTextureSnapshot&&) noexcept = default;
  RendererGeodeTextureSnapshot& operator=(RendererGeodeTextureSnapshot&& other) noexcept;

  [[nodiscard]] RendererTextureSnapshotBackend backend() const override {
    return RendererTextureSnapshotBackend::Geode;
  }
  [[nodiscard]] Vector2i dimensions() const override { return dimensions_; }
  [[nodiscard]] AlphaType alphaType() const override { return AlphaType::Premultiplied; }
  [[nodiscard]] RendererBitmap takeSnapshot() const override;

  /// Resolved single-sample WebGPU texture.
  [[nodiscard]] const wgpu::Texture& texture() const { return texture_; }

  /// Lazily-created texture view suitable for ImGui_ImplWGPU's ImTextureID.
  [[nodiscard]] const wgpu::TextureView& textureView() const;

  /// WebGPU texture format.
  [[nodiscard]] wgpu::TextureFormat format() const { return format_; }

private:
  friend class RendererGeode;

  /// Construct a frame-local view that does not retain or release the texture.
  static RendererGeodeTextureSnapshot BorrowCurrentFrame(wgpu::Texture texture, Vector2i dimensions,
                                                         wgpu::TextureFormat format);

  void destroyOwnedBacking() noexcept;

  std::shared_ptr<geode::GeodeDevice> device_;
  geode::ScopedWgpuHandle<wgpu::Texture> ownedTexture_;
  wgpu::Texture texture_;
  mutable geode::ScopedWgpuHandle<wgpu::TextureView> textureView_;
  Vector2i dimensions_ = Vector2i::Zero();
  wgpu::TextureFormat format_ = wgpu::TextureFormat::Undefined;
};

/**
 * Per-frame performance instrumentation for RendererGeode.
 *
 * Returned by `RendererGeode::lastFrameTimings()`. Each field reports the
 * cost of the most recent `beginFrame`→`endFrame` window. Counters are the
 * durable CI signal; the GPU-timestamp fields are advisory and require
 * `enableTimestamps(true)` + driver support (see `GeodeDevice`).
 *
 * See `docs/design_docs/0030-geode_performance.md` for the target ceilings
 * each optimization milestone drives these toward.
 */
struct FrameTimings {
  /// Counters for resource creation and command submission in the last
  /// frame. Available regardless of timestamp support.
  geode::GeodeCounters counters;

  /// GPU render-pass duration in nanoseconds. Zero if timestamps are
  /// disabled or unsupported by the driver. Reserved for future work -
  /// currently always zero.
  uint64_t renderPassNs = 0;

  /// Total GPU work duration in nanoseconds (end of beginFrame's first
  /// submit to completion of endFrame's final submit). Zero if timestamps
  /// are disabled or unsupported. Reserved for future work - currently
  /// always zero.
  uint64_t totalGpuNs = 0;
};

/**
 * Retained-memory instrumentation for RendererGeode's transient texture pool.
 *
 * Counts include only free textures available for cross-frame reuse, not textures currently in
 * use by an active frame or the renderer's primary target.
 */
struct RendererGeodeTexturePoolStats {
  /// Number of reusable textures currently retained by the pool.
  std::size_t textureCount = 0;

  /// Number of non-empty exact-size texture buckets.
  std::size_t bucketCount = 0;

  /// Estimated GPU bytes retained by reusable textures.
  uint64_t bytes = 0;

  /// Hard upper bound for retained bytes.
  uint64_t budgetBytes = 0;
};

/**
 * Geode rendering backend - GPU-native via WebGPU + the Slug algorithm.
 *
 * `RendererGeode` implements `RendererInterface` by translating draw calls
 * into the lower-level `donner::geode::GeoEncoder` API.
 *
 * ## Construction modes
 *
 * | Mode | Constructor | Device ownership |
 * |------|-------------|-----------------|
 * | Headless | `RendererGeode(verbose)` | Geode leases an exclusive pooled device |
 * | Shared | `RendererGeode(shared_ptr<GeodeDevice>)` | Caller shares ownership |
 *
 * In all modes, Geode creates its own offscreen render target each frame
 * unless a host-owned target is set via `setTargetTexture()`.
 *
 * ## Embedded rendering
 *
 * Host applications that already own a WebGPU device can:
 * 1. Create a `GeodeDevice` from their existing device via
 *    `GeodeDevice::CreateFromExternal(GeodeEmbedConfig{...})`.
 * 2. Optionally call `setTargetTexture()` to render directly into a
 *    swap-chain texture or other host-owned surface.
 * 3. Call `draw()` or the `beginFrame()`/`endFrame()` lifecycle as usual.
 *
 * If `GeodeDevice::CreateHeadless()` fails (no GPU available), all draw
 * operations become no-ops and `takeSnapshot()` returns an empty bitmap.
 */
class RendererGeode : public RendererInterface {
public:
  /**
   * Construct the renderer. Acquires an exclusive lease on a pooled headless `GeodeDevice`; if
   * device creation fails, the renderer enters a "no-op" state and all subsequent draw calls do
   * nothing. Pooling avoids repeated physical-device creation and keeps WebGPU pipeline caches warm
   * for sequential renderer instances, while concurrently live renderers receive independent
   * devices. Callers that intentionally share a device should use the explicit-device constructor.
   *
   * @param verbose If true, emit warnings to stderr for unsupported features
   *   the first time they are encountered.
   */
  explicit RendererGeode(bool verbose = false);

  /**
   * Construct the renderer with an externally-owned `GeodeDevice`.
   *
   * The caller retains shared ownership of the device; it must outlive every
   * frame rendered through this renderer. This overload avoids creating a new
   * WebGPU instance/adapter/device per renderer, which is important in test
   * fixtures that construct thousands of short-lived renderers - Mesa llvmpipe
   * (and some Intel ANV configurations) accumulate driver state across device
   * creations and eventually deadlock.
   *
   * @param device Shared device. Must not be null.
   * @param verbose If true, emit warnings for unsupported features.
   */
  explicit RendererGeode(std::shared_ptr<geode::GeodeDevice> device, bool verbose = false);

  ~RendererGeode() override;

  RendererGeode(const RendererGeode&) = delete;
  RendererGeode& operator=(const RendererGeode&) = delete;
  /// Move constructor.
  RendererGeode(RendererGeode&&) noexcept;
  /// Move assignment operator.
  RendererGeode& operator=(RendererGeode&&) noexcept;

  // --- Embedded rendering ---

  /**
   * Set a host-owned texture as the render target for subsequent frames.
   *
   * When a target texture is set, `beginFrame()` renders into it instead of
   * allocating an internal offscreen target. The texture must:
   * - Have `wgpu::TextureUsage::RenderAttachment` set.
   * - Match the texture format configured on the `GeodeDevice` (default:
   *   `RGBA8Unorm`).
   * - Be at least as large as the viewport (in device pixels).
   *
   * If the texture also has `CopySrc` usage, `takeSnapshot()` can read it back.
   * If it lacks `CopySrc`, `takeSnapshot()` returns an empty bitmap.
   *
   * The host retains ownership of the texture; it must remain valid from
   * `beginFrame()` through `endFrame()`. Call `clearTargetTexture()` to
   * revert to internal offscreen targets.
   *
   * @param texture Host-owned render target texture. Must not be null.
   */
  void setTargetTexture(wgpu::Texture texture);

  /// Clear a previously set target texture, reverting to internal offscreen
  /// targets allocated per-frame by `beginFrame()`.
  void clearTargetTexture();

  /**
   * Preserve the host-owned target contents when the next frame begins.
   *
   * This is for embedded append passes: the host has already rendered into the
   * target texture, and Geode should draw additional renderer primitives on top
   * instead of clearing the texture.
   *
   * @param preserve True to use `LoadOp::Load` for the first render pass.
   */
  void setPreserveTargetOnBeginFrame(bool preserve) override;

  /// Enable analytic edge anti-aliasing. Disabled mode emits binary
  /// pixel-center coverage for deterministic ASCII snapshot tests.
  /// @param antialias True to retain analytic edge coverage.
  void setAntialias(bool antialias);

  // --- RendererInterface ---

  void draw(SVGDocument& document) override;

  [[nodiscard]] int width() const override;
  [[nodiscard]] int height() const override;

  void beginFrame(const RenderViewport& viewport) override;
  void endFrame() override;

  bool drawCheckerboardUnderlay(const CheckerboardUnderlayParams& params) override;

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
  bool drawTextureSnapshot(const RendererTextureSnapshot& texture, const Box2d& targetRect,
                           double opacity = 1.0, bool pixelated = false) override;
  void drawText(Registry& registry, const components::ComputedTextComponent& text,
                const TextParams& params) override;

  [[nodiscard]] std::unique_ptr<RendererInterface> createOffscreenInstance() const override;

  [[nodiscard]] RendererBitmap takeSnapshot() const override;
  [[nodiscard]] RendererBitmap takeSnapshotInterruptibly(
      const std::function<bool()>& shouldCancel) const override;

  [[nodiscard]] RendererReadbackStats consumeReadbackStats() override;

  /**
   * Enable or disable GPU timestamp capture. No-op today; reserved for
   * future work (design doc 0030, "Future Work"). When wired up, this
   * will drive the `renderPassNs` / `totalGpuNs` fields of
   * `lastFrameTimings()`. Counters (the primary regression signal) are
   * always on regardless of this flag.
   */
  void enableTimestamps(bool enabled);

  /**
   * Enable or disable the Geode geometry debug overlay.
   *
   * When enabled, an observer on every path-capable `GeoEncoder` records at
   * the four actual Slug GPU submission paths (gradient, mask, resident fill,
   * and transient/instanced fill). For each instance it reconstructs the two
   * submitted triangles from the six `EncodedPath::quadVertices`, including
   * the shader's dynamic pixel dilation and all submitted transforms. Text,
   * strokes, clips, masks, and instanced paths are therefore included.
   * Pattern-tile resource internals are deliberately excluded; the consuming
   * pattern fill's path submission is captured instead.
   *
   * `endFrame()` draws the collected one-device-pixel opaque-magenta edges in
   * one final root-target pass. Normal document pixels between edges remain
   * unchanged, and later SVG paint, filters, masks, or opacity cannot cover or
   * distort the wireframe. Resource and compositor offscreen instances do not
   * inherit the flag.
   *
   * Default off. The focused regression verifies that explicitly-off output
   * is byte-identical to the default-off renderer. No sink or capture storage
   * is installed; the only hot-path cost is one null-sink branch at each
   * actual Slug submission. Normal batching and `<use>` instancing remain
   * enabled in debug mode; the observer expands submitted instances for the
   * wireframe and the final overlay contributes one additional draw call.
   */
  void setDebugGeometryOverlay(bool enabled) override;

  /// Whether the geometry debug overlay is enabled.
  [[nodiscard]] bool debugGeometryOverlay() const override;

  /**
   * Returns per-frame instrumentation for the most recently completed
   * `beginFrame`→`endFrame` window. Valid after the first `endFrame()`;
   * before then all fields are zero.
   */
  [[nodiscard]] FrameTimings lastFrameTimings() const;

  /// Returns retained-memory instrumentation for the transient texture pool.
  [[nodiscard]] RendererGeodeTexturePoolStats texturePoolStats() const;

  /**
   * Captures the current resolved render target as a directly sampleable
   * WebGPU texture.
   *
   * For internally-owned render targets, this transfers ownership out of the
   * renderer and detaches the current target so a subsequent same-size frame
   * cannot overwrite a texture still being sampled by editor presentation.
   */
  [[nodiscard]] std::shared_ptr<const RendererTextureSnapshot> takeTextureSnapshot() override;

  /// Borrows the current render target until the next frame mutation.
  [[nodiscard]] const RendererTextureSnapshot* borrowTextureSnapshot()
      UTILS_LIFETIME_BOUND override;

  /// Geode presentation is GPU-native when callers can sample WebGPU textures directly.
  ///
  /// Not on the browser build. WebGPU has no cross-thread device, surface, or texture sharing in
  /// any shipping engine, so a texture produced on the raster thread cannot be sampled by the app
  /// thread's device. Browser tiles therefore cross the thread boundary as CPU bitmaps and are
  /// uploaded once per tile generation into the compositing device (single-canvas presenter architecture). The worker-owned
  /// surface that used to consume a texture snapshot directly is gone.
  [[nodiscard]] bool requiresTextureSnapshotPresentation() const override {
#ifdef __EMSCRIPTEN__
    return false;
#else
    return true;
#endif
  }

  /// True whenever this renderer owns a device. An element thumbnail is drawn through an offscreen
  /// instance that shares that same device, so the resulting texture is directly sampleable by
  /// anything already drawing with it. That holds on the browser build too: the cross-thread
  /// limitation above forces CPU bitmaps for the *frame* handoff between the raster thread's
  /// device and the app thread's device, and says nothing about a thumbnail the caller renders on
  /// its own thread and device.
  [[nodiscard]] bool supportsElementTextureSnapshots() const override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace donner::svg
