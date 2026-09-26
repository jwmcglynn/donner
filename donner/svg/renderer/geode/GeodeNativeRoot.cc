#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

#include "donner/base/StringUtils.h"
#include "donner/base/Utils.h"
#include "donner/gpu/GpuLimits.h"
#include "donner/svg/renderer/geode/GeodeWgpuAdapterDevice.h"
#if defined(__APPLE__)
#include "donner/gpu/metal/MetalDevice.h"
#endif
#if defined(__linux__)
#include "donner/gpu/vulkan/VulkanDevice.h"
#endif

namespace donner::geode {

namespace {

std::string_view ProcessBackendRequest() {
  const char* value = std::getenv("DONNER_GPU_BACKEND");
  return value != nullptr ? std::string_view(value) : std::string_view();
}

GpuBackendKind PlatformDefaultGpuBackendKind() {
#if defined(__APPLE__)
  return GpuBackendKind::NativeMetal;
#elif defined(__linux__)
  return GpuBackendKind::NativeVulkan;
#else
  return GpuBackendKind::TransitionalWgpu;
#endif
}

enum class BackendRequestSource : uint8_t { Caller, Environment, BuildSetting, Default };

struct ResolvedBackend {
  GpuBackendKind kind;
  BackendRequestSource source;
};

gpu::Result<GpuBackendKind> ParseBackendRequest(std::string_view request) {
  if (request.empty()) {
    return PlatformDefaultGpuBackendKind();
  }
  using namespace std::string_view_literals;
  if (StringUtils::EqualsLowercase(request, "wgpu"sv)) {
    return GpuBackendKind::TransitionalWgpu;
  }
  if (StringUtils::EqualsLowercase(request, "metal"sv)) {
    return GpuBackendKind::NativeMetal;
  }
  if (StringUtils::EqualsLowercase(request, "vulkan"sv)) {
    return GpuBackendKind::NativeVulkan;
  }
  return gpu::GpuError{gpu::GpuErrorType::InvalidDescriptor,
                       std::format("DONNER_GPU_BACKEND={} names no GPU backend; accepted values: "
                                   "wgpu, metal, vulkan",
                                   request)};
}

gpu::Result<ResolvedBackend> ResolveBackend(const GpuRootSelection& options,
                                            std::string_view request,
                                            std::optional<GpuBackendKind> buildDefault) {
  ResolvedBackend resolved{options.compatibleSurface ? GpuBackendKind::TransitionalWgpu
                                                     : PlatformDefaultGpuBackendKind(),
                           BackendRequestSource::Default};
  if (options.backend.has_value()) {
    resolved = {*options.backend, BackendRequestSource::Caller};
  } else if (!request.empty()) {
    gpu::Result<GpuBackendKind> requested = ParseBackendRequest(request);
    if (requested.hasError()) {
      return std::move(requested).error();
    }
    resolved = {requested.result(), BackendRequestSource::Environment};
  } else if (buildDefault.has_value() && !options.compatibleSurface) {
    resolved = {*buildDefault, BackendRequestSource::BuildSetting};
  }
  if (options.requireVulkanPresentation && resolved.kind != GpuBackendKind::NativeVulkan) {
    return gpu::GpuError{gpu::GpuErrorType::InvalidDescriptor,
                         std::format("Vulkan presentation requested, but the {} backend resolved",
                                     GpuBackendKindName(resolved.kind))};
  }
  if (!options.requireVulkanPresentation && !options.requiredVulkanInstanceExtensions.empty()) {
    return gpu::GpuError{gpu::GpuErrorType::InvalidDescriptor,
                         "Vulkan instance extensions require presentation"};
  }
  return resolved;
}

[[noreturn]] void HaltOnUnservableBackendRequest(const std::string& reason) {
  std::fprintf(stderr, "[Geode] %s\n", reason.c_str());
  std::abort();
}

void ReportSelectedBackendOnce(GpuBackendKind kind, BackendRequestSource source,
                               std::string_view request) {
  if (source == BackendRequestSource::Default) {
    return;
  }
  constexpr uint32_t kSourceCount = 4;
  static std::atomic<uint32_t> reported{0};
  const uint32_t pair =
      1u << (static_cast<uint32_t>(kind) * kSourceCount + static_cast<uint32_t>(source));
  if ((reported.fetch_or(pair, std::memory_order_relaxed) & pair) != 0) {
    return;
  }
  const std::string_view name = GpuBackendKindName(kind);
  switch (source) {
    case BackendRequestSource::Caller:
      std::fprintf(stderr, "[Geode] GPU backend: %.*s, named by the caller.\n",
                   static_cast<int>(name.size()), name.data());
      break;
    case BackendRequestSource::Environment:
      std::fprintf(stderr, "[Geode] GPU backend: %.*s, requested by DONNER_GPU_BACKEND=%.*s.\n",
                   static_cast<int>(name.size()), name.data(), static_cast<int>(request.size()),
                   request.data());
      break;
    case BackendRequestSource::BuildSetting:
      std::fprintf(stderr, "[Geode] GPU backend: %.*s, selected by the build setting.\n",
                   static_cast<int>(name.size()), name.data());
      break;
    case BackendRequestSource::Default: break;
  }
}

std::shared_ptr<GeodeGpuRoot> SelectNativeMetalRoot(
    const GpuRootSelection& options, std::shared_ptr<gpu::DeviceLostState> lostState) {
#if defined(__APPLE__)
  if (options.compatibleSurface) {
    std::fprintf(stderr, "[Geode/metal] A WebGPU surface cannot use native Metal.\n");
    return nullptr;
  }
  const auto metal = gpu::metal::MetalDevice::QuerySystemCapabilities();
  if (!metal.has_value()) {
    std::fprintf(stderr, "[Geode/metal] No Metal device available.\n");
    return nullptr;
  }
  GeodeGpuRootCapabilities capabilities;
  capabilities.backend = GpuBackendKind::NativeMetal;
  capabilities.maxTextureDimension2D = metal->maxTextureDimension2D;
  return std::make_shared<GeodeGpuRoot>(GeodeWgpuRoots{}, capabilities, std::move(lostState));
#else
  (void)options;
  (void)lostState;
  std::fprintf(stderr, "[Geode] No native Metal backend on this platform.\n");
  return nullptr;
#endif
}

std::shared_ptr<GeodeGpuRoot> SelectNativeVulkanRoot(
    const GpuRootSelection& options, std::shared_ptr<gpu::DeviceLostState> lostState) {
#if defined(__linux__)
  if (options.compatibleSurface) {
    std::fprintf(stderr, "[Geode/vulkan] A WebGPU surface cannot use native Vulkan.\n");
    return nullptr;
  }
  if (!options.requireVulkanPresentation && !options.requiredVulkanInstanceExtensions.empty()) {
    std::fprintf(stderr, "[Geode/vulkan] Surface extensions need a presentation root.\n");
    return nullptr;
  }
  std::shared_ptr<gpu::vulkan::VulkanSharedRoot> nativeRoot =
      options.requireVulkanPresentation
          ? gpu::vulkan::VulkanDevice::CreateSharedRootWithPresentationSupport(
                options.requiredVulkanInstanceExtensions, lostState)
          : gpu::vulkan::VulkanDevice::CreateSharedRoot(lostState);
  if (nativeRoot == nullptr) {
    std::fprintf(stderr, "[Geode/vulkan] No Vulkan device available.\n");
    return nullptr;
  }
  return AdoptNativeVulkanRoot(std::move(nativeRoot), std::move(lostState));
#else
  (void)options;
  (void)lostState;
  std::fprintf(stderr, "[Geode] No native Vulkan backend on this platform.\n");
  return nullptr;
#endif
}

}  // namespace

GeodeGpuRoot::GeodeGpuRoot(GeodeWgpuRoots handles, GeodeGpuRootCapabilities capabilities,
                           std::shared_ptr<gpu::DeviceLostState> lostState,
                           std::shared_ptr<const void> backendHold,
                           std::shared_ptr<gpu::vulkan::VulkanSharedRoot> vulkanRoot)
    : handles_(std::move(handles)),
      capabilities_(capabilities),
      lostState_(lostState ? std::move(lostState) : std::make_shared<gpu::DeviceLostState>()),
      backendHold_(std::move(backendHold)),
      vulkanRoot_(std::move(vulkanRoot)) {
  UTILS_RELEASE_ASSERT_MSG(!handles_.owned, "Native Geode cannot own WebGPU handles");
}

GeodeGpuRoot::~GeodeGpuRoot() = default;

bool GeodeGpuRoot::names(const wgpu::Instance& instance, const wgpu::Adapter& adapter,
                         const wgpu::Device& device, const wgpu::Queue& queue) const {
  return (!instance ||
          static_cast<WGPUInstance>(instance) == static_cast<WGPUInstance>(handles_.instance)) &&
         (!adapter ||
          static_cast<WGPUAdapter>(adapter) == static_cast<WGPUAdapter>(handles_.adapter)) &&
         (!device || static_cast<WGPUDevice>(device) == static_cast<WGPUDevice>(handles_.device)) &&
         (!queue || static_cast<WGPUQueue>(queue) == static_cast<WGPUQueue>(handles_.queue));
}

bool GeodeGpuRoot::hasBackendDevice() const {
  if (capabilities_.backend == GpuBackendKind::NativeVulkan) {
    return vulkanRoot_ != nullptr;
  }
  return capabilities_.backend == GpuBackendKind::NativeMetal;
}

std::string_view GpuBackendKindName(GpuBackendKind kind) {
  switch (kind) {
    case GpuBackendKind::TransitionalWgpu: return "transitional wgpu adapter";
    case GpuBackendKind::NativeMetal: return "native Metal";
    case GpuBackendKind::NativeVulkan: return "native Vulkan";
    case GpuBackendKind::Browser: return "browser";
  }
  UTILS_UNREACHABLE();
}

std::ostream& operator<<(std::ostream& os, GpuBackendKind kind) {
  return os << GpuBackendKindName(kind);
}

gpu::Result<GpuBackendKind> ProcessDefaultGpuBackendKind() {
  return ParseBackendRequest(ProcessBackendRequest());
}

std::optional<GpuBackendKind> BuildDefaultGpuBackendKind() {
  return std::nullopt;
}

gpu::Result<GpuBackendKind> ResolveGpuBackendKind(const GpuRootSelection& options,
                                                  std::string_view request,
                                                  std::optional<GpuBackendKind> buildDefault) {
  gpu::Result<ResolvedBackend> resolved = ResolveBackend(options, request, buildDefault);
  if (resolved.hasError()) {
    return std::move(resolved).error();
  }
  return resolved.result().kind;
}

std::shared_ptr<GeodeGpuRoot> SelectGpuRoot(const GpuRootSelection& options) {
  const std::string_view request = ProcessBackendRequest();
  gpu::Result<ResolvedBackend> resolved =
      ResolveBackend(options, request, BuildDefaultGpuBackendKind());
  if (resolved.hasError()) {
    HaltOnUnservableBackendRequest(resolved.error().message);
  }
  const GpuBackendKind kind = resolved.result().kind;
  const BackendRequestSource source = resolved.result().source;
  if (kind == GpuBackendKind::TransitionalWgpu || kind == GpuBackendKind::Browser) {
    if (source == BackendRequestSource::Environment) {
      HaltOnUnservableBackendRequest(
          std::format("DONNER_GPU_BACKEND={} is unavailable in this native build", request));
    }
    std::fprintf(stderr, "[Geode] Requested backend is unavailable in this native build.\n");
    return nullptr;
  }
  auto lostState = std::make_shared<gpu::DeviceLostState>();
  std::shared_ptr<GeodeGpuRoot> root = kind == GpuBackendKind::NativeMetal
                                           ? SelectNativeMetalRoot(options, std::move(lostState))
                                           : SelectNativeVulkanRoot(options, std::move(lostState));
  if (root == nullptr) {
    if (source == BackendRequestSource::Environment) {
      HaltOnUnservableBackendRequest(
          std::format("DONNER_GPU_BACKEND={} asked for the {} backend, which this process could "
                      "not select",
                      request, GpuBackendKindName(kind)));
    }
    return nullptr;
  }
  ReportSelectedBackendOnce(kind, source, request);
  return root;
}

std::shared_ptr<GeodeGpuRoot> AdoptGpuRoot(const GeodeWgpuRoots&,
                                           std::shared_ptr<gpu::DeviceLostState>) {
  std::fprintf(stderr, "[Geode] External WebGPU roots are unavailable in this native build.\n");
  return nullptr;
}

std::shared_ptr<GeodeGpuRoot> AdoptNativeVulkanRoot(
    std::shared_ptr<gpu::vulkan::VulkanSharedRoot> nativeRoot,
    std::shared_ptr<gpu::DeviceLostState> lostState) {
#if defined(__linux__)
  if (nativeRoot == nullptr || lostState == nullptr || nativeRoot->lostState() != lostState) {
    return nullptr;
  }
  GeodeGpuRootCapabilities capabilities;
  capabilities.backend = GpuBackendKind::NativeVulkan;
  capabilities.maxTextureDimension2D = nativeRoot->maxTextureDimension2D();
  capabilities.isVulkan = true;
  return std::make_shared<GeodeGpuRoot>(GeodeWgpuRoots{}, capabilities, std::move(lostState),
                                        nullptr, std::move(nativeRoot));
#else
  (void)nativeRoot;
  (void)lostState;
  return nullptr;
#endif
}

GeodeRuntimeDevice CreateGpuDeviceOver(std::shared_ptr<GeodeGpuRoot> root) {
  UTILS_RELEASE_ASSERT(root != nullptr);
  if (root->capabilities().backend == GpuBackendKind::NativeMetal) {
#if defined(__APPLE__)
    return {.device = gpu::metal::MetalDevice::Create(
                gpu::metal::MetalDevice::MemoryModel::Detected, gpu::kMaxBufferByteSize,
                std::chrono::seconds(5), root->lostState())};
#endif
  }
  if (root->capabilities().backend == GpuBackendKind::NativeVulkan) {
#if defined(__linux__)
    return {.device = gpu::vulkan::VulkanDevice::CreateOverSharedRoot(root->vulkanRoot())};
#endif
  }
  return {};
}

std::size_t OutstandingSelectionInstances() {
  return 0;
}
std::size_t OutstandingDeviceLostCallbacks() {
  return 0;
}

wgpu::TextureFormat WgpuTextureFormatFrom(gpu::TextureFormat format) {
  switch (format) {
    case gpu::TextureFormat::RGBA8Unorm: return wgpu::TextureFormat::RGBA8Unorm;
    case gpu::TextureFormat::BGRA8Unorm: return wgpu::TextureFormat::BGRA8Unorm;
    case gpu::TextureFormat::R8Unorm: return wgpu::TextureFormat::R8Unorm;
    case gpu::TextureFormat::RGBA32Float: return wgpu::TextureFormat::RGBA32Float;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated TextureFormat out of range");
  return wgpu::TextureFormat::RGBA8Unorm;
}

gpu::TextureFormat GpuTextureFormatFromWgpu(wgpu::TextureFormat format) {
  switch (static_cast<WGPUTextureFormat>(format)) {
    case WGPUTextureFormat_RGBA8Unorm: return gpu::TextureFormat::RGBA8Unorm;
    case WGPUTextureFormat_BGRA8Unorm: return gpu::TextureFormat::BGRA8Unorm;
    case WGPUTextureFormat_R8Unorm: return gpu::TextureFormat::R8Unorm;
    case WGPUTextureFormat_RGBA32Float: return gpu::TextureFormat::RGBA32Float;
    default: break;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "WebGPU texture format is unsupported by Donner GPU");
  return gpu::TextureFormat::RGBA8Unorm;
}

}  // namespace donner::geode
