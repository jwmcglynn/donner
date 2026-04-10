#pragma once
/// @file
/// RAII wrapper around a headless WebGPU (Dawn) device.

#include <memory>
#include <webgpu/webgpu_cpp.h>

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

private:
  GeodeDevice();

  // Order matters: queue/device/adapter/instance destroy bottom-up.
  struct Impl;
  std::unique_ptr<Impl> impl_;

  // Exposed shortcuts to Impl members (for fast access without indirection).
  wgpu::Adapter adapter_;
  wgpu::Device device_;
  wgpu::Queue queue_;
};

}  // namespace donner::geode
