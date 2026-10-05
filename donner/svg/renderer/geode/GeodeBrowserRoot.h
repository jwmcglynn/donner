#pragma once
/// @file
/// Browser GPU root selection.

#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <string_view>

#include "donner/base/Utils.h"
#include "donner/gpu/Device.h"

namespace donner::geode {

/// The backend selected for a Geode runtime device.
enum class GpuBackendKind : uint8_t {
  NativeMetal,
  NativeVulkan,
  Browser,
};

/// Human-readable name of a selected backend.
/// @param kind Backend kind to name.
std::string_view GpuBackendKindName(GpuBackendKind kind);
/// Prints the kind's human-readable name.
/// @param os Output stream.
/// @param kind Value to output.
std::ostream& operator<<(std::ostream& os, GpuBackendKind kind);

/// Capabilities shared by runtime devices opened over one browser GPU device.
struct GeodeGpuRootCapabilities {
  /// Backend whose runtime devices the root opens.
  GpuBackendKind backend = GpuBackendKind::Browser;
  /// Largest 2D texture width or height the selected device supports.
  uint32_t maxTextureDimension2D = 8192u;
  /// Whether the backend is Vulkan, where filter passes wait for the queue at each submission
  /// boundary instead of relying on cross-submission synchronization.
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

  /// Return whether this root can open runtime devices for its backend.
  bool hasBackendDevice() const { return backendHold_ != nullptr; }

private:
  GeodeGpuRootCapabilities capabilities_;
  std::shared_ptr<gpu::DeviceLostState> lostState_;
  std::shared_ptr<const void> backendHold_;
};

/// Inputs to browser-root selection. A named unsupported backend is refused.
struct GpuRootSelection {
  /// Caller's name for the selection.
  std::string_view label = "GeodeDevice";
  /// Backend the caller names. It outranks `DONNER_GPU_BACKEND` and every default.
  std::optional<GpuBackendKind> backend;
};

/// One logical runtime device over the selected browser root.
struct GeodeRuntimeDevice {
  std::unique_ptr<gpu::Device> device;  //!< The device, or null when none could be opened.
};

/// In WebAssembly builds, the backend `DONNER_GPU_BACKEND` requests, or the browser backend when
/// it is unset or empty.
gpu::Result<GpuBackendKind> ProcessDefaultGpuBackendKind();
/// In WebAssembly builds, the browser backend, selected when nothing else names a backend.
std::optional<GpuBackendKind> BuildDefaultGpuBackendKind();
/// In WebAssembly builds, resolves the caller's backend, then `DONNER_GPU_BACKEND`, then the build
/// default, without opening a device.
gpu::Result<GpuBackendKind> ResolveGpuBackendKind(const GpuRootSelection& options,
                                                  std::string_view request,
                                                  std::optional<GpuBackendKind> buildDefault);
/// In WebAssembly builds, opens the browser's GPU device as a root. When the caller names no
/// backend, halts if `DONNER_GPU_BACKEND` is invalid or names a native backend. A caller-named
/// native backend halts when `DONNER_GPU_BACKEND` is non-empty and returns null otherwise. Also
/// returns null when the device request fails.
std::shared_ptr<GeodeGpuRoot> SelectGpuRoot(const GpuRootSelection& options);
/// In WebAssembly builds, opens one runtime device over the browser root, sharing its loss
/// condition.
GeodeRuntimeDevice CreateGpuDeviceOver(std::shared_ptr<GeodeGpuRoot> root);

}  // namespace donner::geode
