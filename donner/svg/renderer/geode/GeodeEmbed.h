#pragma once
/// @file
/// \c donner::geode::GeodeEmbedConfig - the trusted surface a host embeds Geode through.
///
/// Separate from `GeodeDevice.h` because this is the one place a caller names backend objects:
/// everything else in the Geode device surface is expressed in the GPU runtime's own types, and
/// the header that carries them must not drag the backend headers into every translation unit
/// that only needs a `GeodeDevice*`.

#include <memory>
#include <webgpu/webgpu.hpp>

#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeGpuWait.h"

namespace donner::geode {

/**
 * Configuration for creating a Geode context from existing WebGPU roots.
 *
 * In borrowed mode, the raw root fields are host-owned and must remain valid for the entire
 * lifetime of every context made from this config. In shared-owner mode, `physicalDevice` retains
 * owned or borrowed roots and shared loss state; explicit raw roots and `lostState`, when
 * supplied, must name the same objects.
 *
 * Example:
 * @code
 *   GeodeEmbedConfig config;
 *   config.instance = myInstance;  // Optional; enables browser snapshot callbacks.
 *   config.device = myDevice;
 *   config.queue = myQueue;
 *   config.textureFormat = wgpu::TextureFormat::BGRA8Unorm;
 *
 *   auto geodeDevice = GeodeDevice::CreateFromExternal(config);
 *   RendererGeode renderer(std::move(geodeDevice));
 * @endcode
 *
 * To create multiple logical contexts over one owned physical device, pass the same
 * `physicalDevice` to each config and omit the raw root fields.
 */
struct GeodeEmbedConfig {
  /// Optional host-provided WebGPU instance. May be null when `physicalDevice` supplies it.
  /// Browser embedders should provide it so synchronous snapshot readback can wait for map
  /// callback completion through `Instance::waitAny()`.
  wgpu::Instance instance;

  /// Host-provided WebGPU device. Must not be null unless `physicalDevice` supplies it.
  wgpu::Device device;

  /// Host-provided queue associated with `device`. Must not be null unless `physicalDevice`
  /// supplies it.
  wgpu::Queue queue;

  /// Texture format for render targets. Must match the format of any texture passed to
  /// `RendererGeode::setTargetTexture()`.
  wgpu::TextureFormat textureFormat = wgpu::TextureFormat::RGBA8Unorm;

  /// Optional adapter handle. May be null when `physicalDevice` supplies it. Preserved for hosts
  /// that need to query the adapter associated with the external device.
  wgpu::Adapter adapter;

  /// Optional shared device-lost flag for borrowed raw-root mode. Hosts that install their own
  /// WebGPU device-lost callback should call `gpu::DeclareDeviceLost(*state)`, not store to the
  /// flag directly: the declaration also runs registered device-loss releases. Pass that state
  /// here, so a driver-reported loss on the host device and a bounded-wait timeout inside Geode
  /// converge on the same `GeodeDevice::isDeviceLost()` condition. When null, the GeodeDevice
  /// creates a private flag that only bounded-wait timeouts can set. When `physicalDevice` is set,
  /// this must be null or pointer-identical to the owner's loss state.
  std::shared_ptr<GeodeDeviceLostState> lostState;

  /// Optional shared physical owner. When present, the raw root fields above must either be null
  /// or name the same roots. Appended to preserve legacy positional aggregate initialization.
  std::shared_ptr<GeodePhysicalDeviceOwner> physicalDevice;
};

}  // namespace donner::geode
