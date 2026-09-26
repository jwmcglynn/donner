#pragma once
/// @file
/// Browser GPU root selection without the transitional WebGPU C++ API.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <string_view>

#include "donner/base/Utils.h"
#include "donner/gpu/Device.h"

namespace donner::geode {

class GeodeWgpuAdapterDevice;

/// The backend selected for a Geode runtime device.
enum class GpuBackendKind : uint8_t {
  TransitionalWgpu,
  NativeMetal,
  NativeVulkan,
  Browser,
};

/// Human-readable name of a selected backend.
std::string_view GpuBackendKindName(GpuBackendKind kind);
std::ostream& operator<<(std::ostream& os, GpuBackendKind kind);

/// Capabilities shared by runtime devices opened over one browser GPU device.
struct GeodeGpuRootCapabilities {
  GpuBackendKind backend = GpuBackendKind::Browser;
  uint32_t maxTextureDimension2D = 8192u;
  bool isVulkan = false;
};

/// Owns the browser GPU device and loss condition shared by logical Geode contexts.
class GeodeGpuRoot {
public:
  /// Retain the selected GPU root, its capabilities, and shared device-loss state.
  /// @param capabilities Capabilities exposed by the selected root.
  /// @param lostState Shared loss condition observed by logical devices.
  /// @param backendHold Opaque shared ownership keeping the backend device alive.
  GeodeGpuRoot(GeodeGpuRootCapabilities capabilities,
               std::shared_ptr<gpu::DeviceLostState> lostState,
               std::shared_ptr<const void> backendHold);

  /// Return the selected root's capabilities.
  const GeodeGpuRootCapabilities& capabilities() const UTILS_LIFETIME_BOUND {
    return capabilities_;
  }

  /// Return the shared device-loss state.
  const std::shared_ptr<gpu::DeviceLostState>& lostState() const UTILS_LIFETIME_BOUND {
    return lostState_;
  }

  /// Return whether this root retains a backend device owner.
  bool hasBackendDevice() const { return backendHold_ != nullptr; }

private:
  GeodeGpuRootCapabilities capabilities_;
  std::shared_ptr<gpu::DeviceLostState> lostState_;
  std::shared_ptr<const void> backendHold_;
};

/// Inputs to browser-root selection. A named unsupported backend is refused.
struct GpuRootSelection {
  std::string_view label = "GeodeDevice";
  std::optional<GpuBackendKind> backend;
  bool usePlatformDefaultBackend = true;
};

/// One logical runtime device over the selected browser root.
struct GeodeRuntimeDevice {
  std::unique_ptr<gpu::Device> device;
  GeodeWgpuAdapterDevice* transitionalAdapter = nullptr;
};

gpu::Result<GpuBackendKind> ProcessDefaultGpuBackendKind();
std::optional<GpuBackendKind> BuildDefaultGpuBackendKind();
gpu::Result<GpuBackendKind> ResolveGpuBackendKind(const GpuRootSelection& options,
                                                  std::string_view request,
                                                  std::optional<GpuBackendKind> buildDefault);
std::shared_ptr<GeodeGpuRoot> SelectGpuRoot(const GpuRootSelection& options);
GeodeRuntimeDevice CreateGpuDeviceOver(std::shared_ptr<GeodeGpuRoot> root);
std::size_t OutstandingSelectionInstances();
std::size_t OutstandingDeviceLostCallbacks();

}  // namespace donner::geode
