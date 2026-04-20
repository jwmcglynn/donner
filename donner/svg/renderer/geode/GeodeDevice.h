#pragma once
/// @file
/// RAII wrapper around a headless WebGPU (Dawn) device.

#include <memory>
#include <webgpu/webgpu.hpp>

#include "donner/svg/renderer/geode/GeodeCounters.h"

namespace donner::geode {

/**
 * Owns a headless WebGPU device/queue pair for GPU rendering.
 *
 * GeodeDevice is the entry point to the Geode rendering backend. It creates a
 * Dawn instance, selects a default adapter, and creates a device — all without
 * any window system integration. All rendering happens into offscreen textures.
 *
 * Typical usage:
 *
 *     auto maybeDevice = GeodeDevice::CreateHeadless();
 *     if (!maybeDevice) {
 *       // Dawn initialization failed — likely no GPU available.
 *       return;
 *     }
 *     GeodeDevice& device = *maybeDevice;
 *     wgpu::Texture target = device.device().CreateTexture(...);
 *     // ... render ...
 */
class GeodeDevice {
public:
  /**
   * Create a headless GeodeDevice.
   *
   * @return A valid GeodeDevice on success, or an empty unique_ptr if Dawn
   *   could not create an adapter/device (e.g., no GPU, no driver).
   */
  static std::unique_ptr<GeodeDevice> CreateHeadless();

  /// Destructor releases the device and all GPU resources.
  ~GeodeDevice();

  // Non-copyable, movable.
  GeodeDevice(const GeodeDevice&) = delete;
  GeodeDevice& operator=(const GeodeDevice&) = delete;
  /// Move constructor.
  GeodeDevice(GeodeDevice&&) noexcept;
  /// Move assignment operator.
  GeodeDevice& operator=(GeodeDevice&&) noexcept;

  /// Returns the wgpu::Device. Guaranteed valid for the lifetime of this object.
  const wgpu::Device& device() const { return device_; }

  /// Returns the default queue.
  const wgpu::Queue& queue() const { return queue_; }

  /// Returns the adapter backing this device.
  const wgpu::Adapter& adapter() const { return adapter_; }

  /**
   * Whether to use alpha-coverage AA instead of hardware 4× MSAA with
   * `@builtin(sample_mask)`.
   *
   * Returns true on Intel + Vulkan, where writing `@builtin(sample_mask)`
   * from overlapping band quads hangs Mesa ANV / Xe KMD (observed on
   * Arc A380, Mesa 25.2.8). The alpha-coverage path folds coverage
   * into the fragment color instead of relying on the hardware sample
   * mask, and runs at `sampleCount() == 1` (no MSAA / no resolve) to
   * avoid a second class of flaky MSAA-resolve hangs on the same driver.
   */
  bool useAlphaCoverageAA() const { return useAlphaCoverageAA_; }

  /**
   * MSAA sample count for render pipelines and render-target textures.
   *
   * Returns 1 on the alpha-coverage path (Intel Arc + Vulkan), where
   * MSAA resolve triggers flaky GPU hangs on Mesa ANV / Xe KMD. All
   * other adapters get 4× MSAA with hardware sample-mask AA.
   */
  uint32_t sampleCount() const { return useAlphaCoverageAA_ ? 1u : 4u; }

  /**
   * Install a `GeodeCounters` struct for this device. Non-owning; the
   * caller must keep the struct alive for as long as the device might
   * increment it. Pass `nullptr` to disable instrumentation.
   *
   * See design doc 0030 (geode_performance). All Geode components that
   * hold a `GeodeDevice&` route their per-frame hot-path allocation and
   * submission sites through this hook.
   */
  void setCounters(GeodeCounters* counters) { counters_ = counters; }

  /// Non-owning pointer to the installed counters, or null.
  GeodeCounters* counters() const { return counters_; }

  // Counter increment helpers. Cheap no-op when counters are disabled.
  void countBuffer() const {
    if (counters_) ++counters_->bufferCreates;
  }
  void countBindGroup() const {
    if (counters_) ++counters_->bindgroupCreates;
  }
  void countTexture() const {
    if (counters_) ++counters_->textureCreates;
  }
  void countSubmit() const {
    if (counters_) ++counters_->submits;
  }
  void countPathEncode() const {
    if (counters_) ++counters_->pathEncodes;
  }

  /**
   * Whether the driver supports GPU timestamp queries. Always false
   * today — reserved for future work (design doc 0030, "Future Work").
   */
  bool supportsTimestamps() const { return false; }

  /// @name Shared dummy resources (design doc 0030 Milestone 4.2)
  /// @{
  ///
  /// GeoEncoder's bind groups always include pattern + clip-mask
  /// texture/sampler slots, even when the current draw doesn't
  /// actually use them. Each slot is filled with a 1×1 "identity"
  /// texture when the feature is inactive. Prior to M4.2 every
  /// GeoEncoder instance created its own dummies (two textures per
  /// encoder), which showed up as 2+ `textureCreates` per frame per
  /// push/pop. Caching the dummies on the device — one instance per
  /// GeodeDevice — drops that to zero steady-state.

  /// 1×1 opaque-black RGBA8 dummy. Bound into the pattern slot when
  /// the current draw is solid / gradient (not a pattern). The shader
  /// does not sample from it, but the bind group layout still requires
  /// a valid binding.
  const wgpu::Texture& dummyPatternTexture() const;
  /// View of `dummyPatternTexture()`.
  const wgpu::TextureView& dummyPatternTextureView() const;
  /// Linear-Repeat sampler used for both the dummy and real pattern tiles.
  const wgpu::Sampler& dummyPatternSampler() const;

  /// 1×1 R8Unorm with value `0xFF` (= 1.0 coverage). Bound into the
  /// clip-mask slot when no clip mask is active — the shader
  /// multiplies coverage by this value, so `1.0` is a no-op.
  const wgpu::Texture& dummyClipMaskTexture() const;
  /// View of `dummyClipMaskTexture()`.
  const wgpu::TextureView& dummyClipMaskTextureView() const;
  /// Linear-ClampToEdge sampler used for both the dummy and real clip masks.
  const wgpu::Sampler& dummyClipMaskSampler() const;
  /// @}

private:
  GeodeDevice();

  // Order matters: queue/device/adapter/instance destroy bottom-up.
  struct Impl;
  std::unique_ptr<Impl> impl_;

  // Exposed shortcuts to Impl members (for fast access without indirection).
  wgpu::Adapter adapter_;
  wgpu::Device device_;
  wgpu::Queue queue_;

  bool useAlphaCoverageAA_ = false;
  GeodeCounters* counters_ = nullptr;
};

}  // namespace donner::geode
