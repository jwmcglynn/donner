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

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>
#include <webgpu/webgpu.hpp>

#include "donner/base/Box.h"
#include "donner/base/Transform.h"
#include "donner/gpu/Handles.h"
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
 * on to a later frame. The snapshot carries only owned +1 wgpu references
 * (texture and lazily-created view), NOT a \c donner::gpu handle - see the
 * constructor for the cross-thread rationale. The wgpu surface is derived
 * through the adapter's escape hatch at construction (TEMPORARY - the wgpu
 * surface is deleted in packet 18, ImGui/editor).
 */
class RendererGeodeTextureSnapshot final : public RendererTextureSnapshot {
public:
  /**
   * Construct a Geode texture snapshot. Must be called on the renderer thread
   * that owns `device`'s adapter.
   *
   * Threading: snapshots are handed to the editor UI thread (GlTextureCache,
   * debug-panel thumbnails), which may drop the last `shared_ptr` there while
   * the AsyncRenderer worker still owns the adapter. \c donner::gpu::Device is
   * single-threaded, so a \c gpu::Texture RAII release from the UI thread
   * would race the adapter's slot tables and deferred-destroy queue. wgpu
   * refcounting IS thread-safe, so an owned +1 wgpu reference is the only
   * ownership the snapshot can safely carry across threads. The constructor
   * therefore derives the +1 wgpu reference and releases the \c gpu::Texture
   * handle right here, on the renderer thread. Releasing it while the frame
   * that rendered it is still in flight on the GPU is safe by design (the
   * runtime defers the backend release by submission serial); it is safe from
   * unsubmitted-command validation failures because `takeTextureSnapshot`
   * runs only after `endFrame`'s submit, so no still-unsubmitted recorded
   * commands reference the handle.
   *
   * @param device Shared Geode device that owns the WebGPU handle lifetime.
   * @param texture Resolved single-sample texture containing the rendered frame; consumed by
   *   the constructor (its wgpu backing is retained, the gpu handle is released). Must be a
   *   live texture of `device->adapterDevice()`.
   * @param dimensions Texture dimensions in device pixels.
   * @param format Texture format.
   */
  RendererGeodeTextureSnapshot(std::shared_ptr<geode::GeodeDevice> device, gpu::Texture texture,
                               Vector2i dimensions, wgpu::TextureFormat format);
  ~RendererGeodeTextureSnapshot() override = default;

  [[nodiscard]] RendererTextureSnapshotBackend backend() const override {
    return RendererTextureSnapshotBackend::Geode;
  }
  [[nodiscard]] Vector2i dimensions() const override { return dimensions_; }
  [[nodiscard]] AlphaType alphaType() const override { return AlphaType::Premultiplied; }
  [[nodiscard]] RendererBitmap takeSnapshot() const override;

  /// Resolved single-sample WebGPU texture (TEMPORARY escape-hatch surface for the editor's
  /// ImGui presentation - deleted in packet 18).
  [[nodiscard]] const wgpu::Texture& texture() const { return texture_.get(); }

  /// Lazily-created texture view suitable for ImGui_ImplWGPU's ImTextureID.
  [[nodiscard]] const wgpu::TextureView& textureView() const;

  /// WebGPU texture format.
  [[nodiscard]] wgpu::TextureFormat format() const { return format_; }

private:
  std::shared_ptr<geode::GeodeDevice> device_;
  /// Owned +1 wgpu reference derived from the constructor's `gpu::Texture` so presentation code
  /// keeps a stable wgpu surface; the final release (any thread) is a thread-safe wgpu refcount
  /// decrement. See the constructor comment for why no gpu handle is stored.
  geode::ScopedWgpuHandle<wgpu::Texture> texture_;
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
  void setPreserveTargetOnBeginFrame(bool preserve);

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
   * When enabled, every path draw (fills, strokes, rects, ellipses)
   * additionally outlines the Slug band decomposition Geode emits for
   * that draw: horizontal band strips (cyan), vertical band strips
   * (yellow), and the per-path bounding quad with its triangle diagonal
   * (magenta) - the two triangles Geode actually rasterizes (0041).
   * Overlay draws render through the normal fill pipeline, so they are
   * included in `lastFrameTimings()` counters. `<use>` instancing is
   * disabled while the overlay is on so overlays stay in draw order.
   *
   * Default off. When off, rendering behavior and performance are
   * unchanged; the only cost is one boolean test per draw.
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

  /**
   * Captures the current resolved render target as a directly sampleable
   * WebGPU texture.
   *
   * For internally-owned render targets, this transfers ownership out of the
   * renderer and detaches the current target so a subsequent same-size frame
   * cannot overwrite a texture still being sampled by editor presentation.
   */
  [[nodiscard]] std::shared_ptr<const RendererTextureSnapshot> takeTextureSnapshot() override;

  /// Geode presentation is GPU-native when callers can sample WebGPU textures directly.
  [[nodiscard]] bool requiresTextureSnapshotPresentation() const override {
#ifdef __EMSCRIPTEN__
    // Emscripten's WebGPU object table is per-worker. The editor's async
    // renderer runs on a pthread, while ImGui presents on the main browser
    // thread, so worker-created texture handles cannot be handed directly to
    // ImGui. Use CPU snapshots for the worker handoff in wasm builds.
    return false;
#else
    return true;
#endif
  }

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace donner::svg
