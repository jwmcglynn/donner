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

/// Human-readable name of a selected backend.
/// @param kind Backend kind to name.
std::string_view GpuBackendKindName(GpuBackendKind kind);
/// Prints the kind's human-readable name.
/// @param os Output stream.
/// @param kind Value to output.
std::ostream& operator<<(std::ostream& os, GpuBackendKind kind);

/// Capabilities shared by runtime devices over one native root.
struct GeodeGpuRootCapabilities {
  /// Backend whose runtime devices the root opens.
  GpuBackendKind backend = GpuBackendKind::NativeMetal;
  /// Largest 2D texture width or height the selected device supports.
  uint32_t maxTextureDimension2D = 8192u;
  /// Whether the backend is Vulkan, where filter passes wait for the queue at each submission
  /// boundary instead of relying on cross-submission synchronization.
  bool isVulkan = false;
};

struct GeodeRuntimeDevice;

/// Retains a native GPU owner and the loss condition shared by logical contexts.
class GeodeGpuRoot {
public:
  /// Retains a selected native backend; the external kind is refused, because only an adopted
  /// \ref GeodeRuntimeDeviceSource can open its devices.
  /// @param capabilities Capabilities of the selected native backend.
  /// @param lostState Non-null loss condition shared by logical devices.
  /// @param vulkanRoot Shared Vulkan physical owner; null unless the backend is native Vulkan.
  GeodeGpuRoot(GeodeGpuRootCapabilities capabilities,
               std::shared_ptr<gpu::DeviceLostState> lostState,
               std::shared_ptr<gpu::vulkan::VulkanSharedRoot> vulkanRoot = nullptr);

  /// Return the selected root's capabilities.
  const GeodeGpuRootCapabilities& capabilities() const UTILS_LIFETIME_BOUND {
    return capabilities_;
  }
  /// Return the shared device-loss state.
  const std::shared_ptr<gpu::DeviceLostState>& lostState() const UTILS_LIFETIME_BOUND {
    return lostState_;
  }
  /// Return the retained Vulkan owner; null unless the root is native Vulkan.
  const std::shared_ptr<gpu::vulkan::VulkanSharedRoot>& vulkanRoot() const UTILS_LIFETIME_BOUND {
    return vulkanRoot_;
  }
  /// Return whether this root can open runtime devices for its backend.
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
  /// Caller's name for the selection.
  std::string_view label = "GeodeDevice";
  /// Require a Vulkan root whose queue can present; another resolved backend is a contradiction.
  bool requireVulkanPresentation = false;
  /// Instance extensions a presentation root must enable, such as the window system's surface
  /// extensions. Only valid with \ref requireVulkanPresentation.
  std::span<const char* const> requiredVulkanInstanceExtensions;
  /// Backend the caller names. It outranks `DONNER_GPU_BACKEND` and every default.
  std::optional<GpuBackendKind> backend;
};

/// One logical runtime device over the selected native root.
struct GeodeRuntimeDevice {
  std::unique_ptr<gpu::Device> device;  //!< The device, or null when none could be opened.
};

/**
 * The backend `DONNER_GPU_BACKEND` requests: `metal` or `vulkan` in any letter case, or the
 * platform default (native Metal on Apple, native Vulkan on Linux) when the variable is unset or
 * empty.
 *
 * @return The kind, or an error when the variable names no backend or the platform has no native
 *   backend.
 */
gpu::Result<GpuBackendKind> ProcessDefaultGpuBackendKind();

/**
 * The backend this build selects when neither the caller nor `DONNER_GPU_BACKEND` names one.
 *
 * @return Empty in native builds, which use the platform default.
 */
std::optional<GpuBackendKind> BuildDefaultGpuBackendKind();

/**
 * The backend \ref donner::geode::SelectGpuRoot "SelectGpuRoot" opens for @p options, without
 * opening it. The first of these that applies decides: the backend the caller names, the one
 * @p request names, @p buildDefault, and the platform default.
 *
 * @param options Caller-supplied inputs.
 * @param request Value of `DONNER_GPU_BACKEND`; empty when it is unset or empty.
 * @param buildDefault Backend the build selects when nothing else names one.
 * @return The kind, or an error when the caller names no backend and a non-empty @p request names
 *   none, the external kind is named, no backend applies on this platform, or the Vulkan
 *   presentation options are inconsistent with each other or with the resolved backend.
 */
gpu::Result<GpuBackendKind> ResolveGpuBackendKind(const GpuRootSelection& options,
                                                  std::string_view request,
                                                  std::optional<GpuBackendKind> buildDefault);

/**
 * Selects a native backend root for @p options and opens the system device behind it.
 *
 * Halts the process when the Vulkan presentation options are inconsistent with each other or with
 * the resolved backend. When the caller names no backend and `DONNER_GPU_BACKEND` controls
 * selection, it also halts if the variable names no backend or one this process cannot open: an
 * explicit process request is never silently replaced with another backend.
 *
 * @param options Caller-supplied inputs.
 * @return The root, or null when a caller-named or platform-default backend cannot be opened or
 *   this build cannot serve it.
 */
std::shared_ptr<GeodeGpuRoot> SelectGpuRoot(const GpuRootSelection& options);

/**
 * Adopts a completed native Vulkan root, for example one an embedder selected for its actual
 * presentation surface.
 *
 * @param nativeRoot Completed Vulkan instance, physical and logical device, and queue.
 * @param lostState Loss condition @p nativeRoot was opened with.
 * @return The root, or null when either input is null, @p lostState is not the condition
 *   @p nativeRoot shares, or this platform has no native Vulkan backend.
 */
std::shared_ptr<GeodeGpuRoot> AdoptNativeVulkanRoot(
    std::shared_ptr<gpu::vulkan::VulkanSharedRoot> nativeRoot,
    std::shared_ptr<gpu::DeviceLostState> lostState);

/**
 * Opens one runtime device over @p root. Every logical rendering context gets its own, with its
 * own handle tables and submission serials, while all of them share the root's loss condition.
 *
 * @param root Root to render through; must not be null.
 * @return The device, or an empty device when the backend could not open one.
 */
GeodeRuntimeDevice CreateGpuDeviceOver(std::shared_ptr<GeodeGpuRoot> root);

}  // namespace donner::geode
