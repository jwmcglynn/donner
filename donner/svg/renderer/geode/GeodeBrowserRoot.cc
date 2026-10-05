#include "donner/svg/renderer/geode/GeodeBrowserRoot.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "donner/base/StringUtils.h"
#include "donner/gpu/browser/BrowserDevice.h"
#include "donner/gpu/browser/EmscriptenBrowserBridge.h"

namespace donner::geode {

namespace {

constexpr double kBrowserDeviceSettleSeconds = 10.0;

void ReportAcquisitionFailure(const char* stage, const char* outcome) {
  std::fprintf(stderr, "[Geode/browser/acquire] stage=%s outcome=%s\n", stage, outcome);
}

const char* RequestFailureOutcome(const gpu::browser::BrowserDeviceRequest& request,
                                  gpu::browser::BrowserDeviceRequestState state) {
  if (state == gpu::browser::BrowserDeviceRequestState::Unavailable) {
    return "api_unavailable";
  }
  const std::string reason = request.error().str();
  if (reason == "adapter_null") {
    return "adapter_null";
  }
  if (reason == "adapter_rejected") {
    return "adapter_rejected";
  }
  if (reason == "device_rejected") {
    return "device_rejected";
  }
  if (reason == "device_install_failed") {
    return "device_install_failed";
  }
  return "request_failed";
}

gpu::Result<std::unique_ptr<gpu::browser::BrowserDevice>> OpenBrowserDevice(
    std::shared_ptr<gpu::DeviceLostState> lostState, const char* stage) {
  gpu::browser::BrowserDeviceRequest request = gpu::browser::BrowserDeviceRequest::Begin(
      std::make_unique<gpu::browser::EmscriptenBrowserBridge>());
  const gpu::browser::BrowserDeviceRequestState state = request.settle(kBrowserDeviceSettleSeconds);
  if (state == gpu::browser::BrowserDeviceRequestState::Pending) {
    ReportAcquisitionFailure(stage, "deadline_pending");
    return gpu::GpuError{
        gpu::GpuErrorType::InvalidState,
        std::format("the browser did not settle its GPU device request within {} seconds",
                    kBrowserDeviceSettleSeconds)};
  }
  if (state != gpu::browser::BrowserDeviceRequestState::Ready) {
    ReportAcquisitionFailure(stage, RequestFailureOutcome(request, state));
  }
  gpu::Result<std::unique_ptr<gpu::browser::BrowserDevice>> result =
      std::move(request).take(std::move(lostState));
  if (result.hasError() && state == gpu::browser::BrowserDeviceRequestState::Ready) {
    ReportAcquisitionFailure(stage, "context_failed");
  }
  return result;
}

std::string_view ProcessBackendRequest() {
  const char* value = std::getenv("DONNER_GPU_BACKEND");
  return value != nullptr ? std::string_view(value) : std::string_view();
}

}  // namespace

GeodeGpuRoot::GeodeGpuRoot(GeodeGpuRootCapabilities capabilities,
                           std::shared_ptr<gpu::DeviceLostState> lostState,
                           std::shared_ptr<const void> backendHold)
    : capabilities_(capabilities),
      lostState_(std::move(lostState)),
      backendHold_(std::move(backendHold)) {
  UTILS_RELEASE_ASSERT(lostState_ != nullptr && backendHold_ != nullptr);
}

std::string_view GpuBackendKindName(GpuBackendKind kind) {
  switch (kind) {
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
  return ResolveGpuBackendKind({}, ProcessBackendRequest(), std::nullopt);
}

std::optional<GpuBackendKind> BuildDefaultGpuBackendKind() {
  return GpuBackendKind::Browser;
}

gpu::Result<GpuBackendKind> ResolveGpuBackendKind(const GpuRootSelection& options,
                                                  std::string_view request,
                                                  std::optional<GpuBackendKind> buildDefault) {
  if (options.backend.has_value()) {
    return *options.backend;
  }
  if (!request.empty()) {
    using namespace std::string_view_literals;
    if (StringUtils::EqualsLowercase(request, "metal"sv)) {
      return GpuBackendKind::NativeMetal;
    }
    if (StringUtils::EqualsLowercase(request, "vulkan"sv)) {
      return GpuBackendKind::NativeVulkan;
    }
    return gpu::GpuError{gpu::GpuErrorType::InvalidDescriptor,
                         std::format("DONNER_GPU_BACKEND={} names no GPU backend; accepted values: "
                                     "metal, vulkan",
                                     request)};
  }
  // The browser device is the only one a WebAssembly build can select.
  return buildDefault.value_or(GpuBackendKind::Browser);
}

std::shared_ptr<GeodeGpuRoot> SelectGpuRoot(const GpuRootSelection& options) {
  const std::string_view request = ProcessBackendRequest();
  gpu::Result<GpuBackendKind> kind =
      ResolveGpuBackendKind(options, request, BuildDefaultGpuBackendKind());
  if (kind.hasError()) {
    std::fprintf(stderr, "[Geode] %s\n", kind.error().message.c_str());
    std::abort();
  }
  if (kind.result() != GpuBackendKind::Browser) {
    if (!request.empty()) {
      std::fprintf(stderr,
                   "[Geode] DONNER_GPU_BACKEND=%.*s asked for a backend this WebAssembly "
                   "build cannot select\n",
                   static_cast<int>(request.size()), request.data());
      std::abort();
    }
    return nullptr;
  }
  auto lostState = std::make_shared<gpu::DeviceLostState>();
  gpu::Result<std::unique_ptr<gpu::browser::BrowserDevice>> hold =
      OpenBrowserDevice(lostState, "selection");
  if (hold.hasError()) {
    return nullptr;
  }
  GeodeGpuRootCapabilities capabilities;
  capabilities.maxTextureDimension2D = hold.result()->maxTextureDimension2D();
  const bool namedByCaller = options.backend.has_value();
  const uint8_t sourceBit = namedByCaller ? 1u : 2u;
  static std::atomic<uint8_t> reportedSources{0};
  if ((reportedSources.fetch_or(sourceBit, std::memory_order_relaxed) & sourceBit) == 0) {
    if (namedByCaller) {
      std::fprintf(stderr, "[Geode] GPU backend: browser, named by the caller.\n");
    } else {
      std::fprintf(stderr,
                   "[Geode] GPU backend: browser, selected by the build setting "
                   "//donner/svg/renderer/geode:browser_backend for this WebAssembly build.\n");
    }
  }
  return std::make_shared<GeodeGpuRoot>(capabilities, std::move(lostState),
                                        std::shared_ptr<const void>(std::move(hold).result()));
}

GeodeRuntimeDevice CreateGpuDeviceOver(std::shared_ptr<GeodeGpuRoot> root) {
  UTILS_RELEASE_ASSERT(root != nullptr);
  gpu::Result<std::unique_ptr<gpu::browser::BrowserDevice>> device =
      OpenBrowserDevice(root->lostState(), "runtime");
  if (device.hasError()) {
    return {};
  }
  return GeodeRuntimeDevice{.device = std::move(device).result()};
}

}  // namespace donner::geode
