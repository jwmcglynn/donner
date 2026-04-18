#pragma once
/// @file
/// RAII wrapper around a headless WebGPU (Dawn) device.

#include <memory>
#include <webgpu/webgpu.hpp>

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
   * Arc A380, Mesa 25.2.8). The alpha-coverage path folds 4-sample
   * coverage into the fragment color instead of relying on the hardware
   * sample mask. The pipeline still uses `multisample.count = 4` so
   * MSAA render targets and resolve steps are unchanged.
   */
  bool useAlphaCoverageAA() const { return useAlphaCoverageAA_; }

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
};

}  // namespace donner::geode
