#pragma once
/// @file
/// Native Geode root selection.

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

class GeodeRuntimeDeviceSource;

/// The backend selected for a Geode runtime device.
enum class GpuBackendKind : uint8_t {
  NativeMetal,
  NativeVulkan,
  Browser,
  /// Runtime devices a \ref GeodeRuntimeDeviceSource opens. Never selected: only adopting a
  /// source produces it.
  External,
};

std::string_view GpuBackendKindName(GpuBackendKind kind);
std::ostream& operator<<(std::ostream& os, GpuBackendKind kind);

/// Capabilities shared by runtime devices over one native root.
struct GeodeGpuRootCapabilities {
  GpuBackendKind backend = GpuBackendKind::NativeMetal;
  uint32_t maxTextureDimension2D = 8192u;
  bool isVulkan = false;
};

struct GeodeRuntimeDevice;

/// Retains a native GPU owner and the loss condition shared by logical contexts.
class GeodeGpuRoot {
public:
  /// Retains a selected native backend; the external kind is refused, because only an adopted
  /// \ref GeodeRuntimeDeviceSource can open its devices.
  GeodeGpuRoot(GeodeGpuRootCapabilities capabilities,
               std::shared_ptr<gpu::DeviceLostState> lostState,
               std::shared_ptr<gpu::vulkan::VulkanSharedRoot> vulkanRoot = nullptr);

  const GeodeGpuRootCapabilities& capabilities() const UTILS_LIFETIME_BOUND {
    return capabilities_;
  }
  const std::shared_ptr<gpu::DeviceLostState>& lostState() const UTILS_LIFETIME_BOUND {
    return lostState_;
  }
  const std::shared_ptr<gpu::vulkan::VulkanSharedRoot>& vulkanRoot() const UTILS_LIFETIME_BOUND {
    return vulkanRoot_;
  }
  bool hasBackendDevice() const;

private:
  friend std::shared_ptr<GeodeGpuRoot> AdoptRuntimeDeviceSource(
      std::shared_ptr<GeodeRuntimeDeviceSource> source,
      const GeodeGpuRootCapabilities& capabilities,
      std::shared_ptr<gpu::DeviceLostState> lostState);
  friend GeodeRuntimeDevice CreateGpuDeviceOver(std::shared_ptr<GeodeGpuRoot> root);

  GeodeGpuRoot(GeodeGpuRootCapabilities capabilities,
               std::shared_ptr<gpu::DeviceLostState> lostState,
               std::shared_ptr<GeodeRuntimeDeviceSource> deviceSource);

  GeodeGpuRootCapabilities capabilities_;
  std::shared_ptr<gpu::DeviceLostState> lostState_;
  std::shared_ptr<gpu::vulkan::VulkanSharedRoot> vulkanRoot_;
  /// Opens the runtime devices of an external root; null for a selected backend.
  std::shared_ptr<GeodeRuntimeDeviceSource> deviceSource_;
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

}  // namespace donner::geode
