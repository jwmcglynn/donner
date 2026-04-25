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
#include "donner/svg/SVGDocument.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/geode/GeodeCounters.h"

namespace donner::geode {
class GeodeDevice;
class GeodePipeline;
class GeoEncoder;
struct GeodeEmbedConfig;
}  // namespace donner::geode

// Forward-declare std::shared_ptr specialization to avoid pulling <memory>
// into every includer — <memory> is already included above.

namespace donner::svg {

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
  /// disabled or unsupported by the driver. Reserved for future work —
  /// currently always zero.
  uint64_t renderPassNs = 0;

  /// Total GPU work duration in nanoseconds (end of beginFrame's first
  /// submit to completion of endFrame's final submit). Zero if timestamps
  /// are disabled or unsupported. Reserved for future work — currently
  /// always zero.
  uint64_t totalGpuNs = 0;
};

/**
 * Geode rendering backend — GPU-native via WebGPU + the Slug algorithm.
 *
 * `RendererGeode` implements `RendererInterface` by translating draw calls
 * into the lower-level `donner::geode::GeoEncoder` API.
 *
 * ## Construction modes
 *
 * | Mode | Constructor | Device ownership |
 * |------|-------------|-----------------|
 * | Headless | `RendererGeode(verbose)` | Geode creates + owns device |
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
   * Construct the renderer. Creates a headless `GeodeDevice` immediately;
   * if device creation fails, the renderer enters a "no-op" state and all
   * subsequent draw calls do nothing.
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
   * fixtures that construct thousands of short-lived renderers — Mesa llvmpipe
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

  [[nodiscard]] std::unique_ptr<RendererInterface> createOffscreenInstance() const override;

  [[nodiscard]] RendererBitmap takeSnapshot() const override;

  /**
   * Capture the render target's premultiplied bytes directly, skipping the
   * unpremultiply conversion `takeSnapshot()` applies for cross-backend
   * parity. The compositor uses this to avoid a lossy 8-bit conversion
   * round-trip through its internal layer bitmap cache.
   */
  [[nodiscard]] RendererBitmap takeSnapshotPremultiplied() const override;

  /**
   * Enable or disable GPU timestamp capture. No-op today; reserved for
   * future work (design doc 0030, "Future Work"). When wired up, this
   * will drive the `renderPassNs` / `totalGpuNs` fields of
   * `lastFrameTimings()`. Counters (the primary regression signal) are
   * always on regardless of this flag.
   */
  void enableTimestamps(bool enabled);

  /**
   * Returns per-frame instrumentation for the most recently completed
   * `beginFrame`→`endFrame` window. Valid after the first `endFrame()`;
   * before then all fields are zero.
   */
  [[nodiscard]] FrameTimings lastFrameTimings() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace donner::svg
