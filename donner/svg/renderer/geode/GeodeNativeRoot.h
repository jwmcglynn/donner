#pragma once
/// @file
/// Native Geode root selection without WebGPU C or C++ declarations.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string_view>

#include "donner/base/Utils.h"
#include "donner/gpu/Device.h"

namespace donner::gpu::vulkan {
class VulkanSharedRoot;
}

namespace donner::geode {

/// The backend selected for a Geode runtime device.
enum class GpuBackendKind : uint8_t {
  TransitionalWgpu,
  NativeMetal,
  NativeVulkan,
  Browser,
};

std::string_view GpuBackendKindName(GpuBackendKind kind);
std::ostream& operator<<(std::ostream& os, GpuBackendKind kind);

/// Capabilities shared by runtime devices over one native root.
struct GeodeGpuRootCapabilities {
  GpuBackendKind backend = GpuBackendKind::NativeMetal;
  uint32_t maxTextureDimension2D = 8192u;
  bool isVulkan = false;
};

/// Retains native backend capabilities, shared loss state, and the Vulkan physical owner.
class GeodeGpuRoot {
public:
  /// Capture backend capabilities and shared root state for logical contexts.
  /// @param capabilities Capabilities of the selected native backend.
  /// @param lostState Non-null loss condition shared by logical devices.
  /// @param vulkanRoot Shared Vulkan physical owner, omitted for Metal.
  GeodeGpuRoot(GeodeGpuRootCapabilities capabilities,
               std::shared_ptr<gpu::DeviceLostState> lostState,
               std::shared_ptr<gpu::vulkan::VulkanSharedRoot> vulkanRoot = nullptr);

  /// Return the selected native backend capabilities.
  const GeodeGpuRootCapabilities& capabilities() const UTILS_LIFETIME_BOUND {
    return capabilities_;
  }
  /// Return the shared device-loss condition.
  const std::shared_ptr<gpu::DeviceLostState>& lostState() const UTILS_LIFETIME_BOUND {
    return lostState_;
  }
  /// Return the retained Vulkan owner; null for a Metal root.
  const std::shared_ptr<gpu::vulkan::VulkanSharedRoot>& vulkanRoot() const UTILS_LIFETIME_BOUND {
    return vulkanRoot_;
  }
  /// Return whether the selected native backend has the required root state.
  bool hasBackendDevice() const;

private:
  GeodeGpuRootCapabilities capabilities_;
  std::shared_ptr<gpu::DeviceLostState> lostState_;
  std::shared_ptr<gpu::vulkan::VulkanSharedRoot> vulkanRoot_;
};

/// Caller inputs for a native backend selection.
struct GpuRootSelection {
  std::string_view label = "GeodeDevice";
  bool requireVulkanPresentation = false;
  std::span<const char* const> requiredVulkanInstanceExtensions;
  std::optional<GpuBackendKind> backend;
};

/// One logical runtime device over the selected native root.
struct GeodeRuntimeDevice {
  std::unique_ptr<gpu::Device> device;
};

gpu::Result<GpuBackendKind> ProcessDefaultGpuBackendKind();
std::optional<GpuBackendKind> BuildDefaultGpuBackendKind();
gpu::Result<GpuBackendKind> ResolveGpuBackendKind(const GpuRootSelection& options,
                                                  std::string_view request,
                                                  std::optional<GpuBackendKind> buildDefault);
std::shared_ptr<GeodeGpuRoot> SelectGpuRoot(const GpuRootSelection& options);
std::shared_ptr<GeodeGpuRoot> AdoptNativeVulkanRoot(
    std::shared_ptr<gpu::vulkan::VulkanSharedRoot> nativeRoot,
    std::shared_ptr<gpu::DeviceLostState> lostState);
GeodeRuntimeDevice CreateGpuDeviceOver(std::shared_ptr<GeodeGpuRoot> root);
std::size_t OutstandingSelectionInstances();
std::size_t OutstandingDeviceLostCallbacks();

}  // namespace donner::geode
