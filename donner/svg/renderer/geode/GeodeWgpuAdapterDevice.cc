// Exactly one translation unit in the binary must define `WEBGPU_CPP_IMPLEMENTATION` before
// including `<webgpu/webgpu.hpp>`. The header ships the body of every C++ wrapper method inside a
// `#ifdef WEBGPU_CPP_IMPLEMENTATION` block; without this define the wrapper methods are declared
// but never defined, and linking fails with unresolved `wgpu::Instance::requestAdapter` and
// friends.
#define WEBGPU_CPP_IMPLEMENTATION
#include "donner/svg/renderer/geode/GeodeWgpuAdapterDevice.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "donner/base/StringUtils.h"
#include "donner/base/Utils.h"
#include "donner/gpu/CheckedArithmetic.h"
#include "donner/gpu/GpuLimits.h"
#include "donner/svg/renderer/geode/GeodeCallbackState.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeGpuWait.h"
#include "donner/svg/renderer/geode/GeodeNativeRoot.h"
#include "donner/svg/renderer/geode/GeodeRuntimeDeviceSource.h"

namespace donner::geode {

namespace {

/// Releases the backend objects a selection created, in the order their ownership nests: the
/// queue and device first, then the adapter and instance they came from. Shared by the root's
/// destructor and by a selection that gives up partway, so one sequence releases them however the
/// selection ends.
/// @param handles Backend objects to release; left null.
void ReleaseSelectedHandles(GeodeWgpuRoots& handles);

/// Releases the backend objects a selection has built so far unless the selection completes.
///
/// Until the root exists there is no other owner of them: every failure exit after the instance
/// is created would otherwise strand an instance, and past the adapter and device requests an
/// adapter, an undestroyed device and a retained device-lost callback as well. Adapter
/// acquisition failing is the routine outcome on a host with no usable GPU, so those exits are
/// taken often rather than exceptionally.
class PartialSelection {
public:
  /// Takes responsibility for \p handles until \ref keep is called.
  /// @param handles Backend objects the selection is filling in.
  explicit PartialSelection(GeodeWgpuRoots& handles) : handles_(&handles) {}

  ~PartialSelection() {
    if (handles_ != nullptr) {
      ReleaseSelectedHandles(*handles_);
    }
  }

  PartialSelection(const PartialSelection&) = delete;
  PartialSelection& operator=(const PartialSelection&) = delete;

  /// Hands the objects to the root that is about to be built, so they outlive this scope.
  void keep() { handles_ = nullptr; }

private:
  GeodeWgpuRoots* handles_;
};

enum class DeviceLostCallbackStatus : std::uint8_t {
  Pending,
  Running,
  Done,
  Canceled,
};

struct DeviceLostCallbackToken {
  explicit DeviceLostCallbackToken(std::shared_ptr<gpu::DeviceLostState> stateIn)
      : state(std::move(stateIn)) {}

  std::atomic<DeviceLostCallbackStatus> status{DeviceLostCallbackStatus::Pending};
  std::atomic<int> references{2};
  std::shared_ptr<gpu::DeviceLostState> state;
};

void ReleaseDeviceLostCallbackTokenReference(DeviceLostCallbackToken* token) {
  if (token->references.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    delete token;
  }
}

void* CreateDeviceLostCallbackToken(const std::shared_ptr<gpu::DeviceLostState>& state) {
  return new DeviceLostCallbackToken(state);
}

std::shared_ptr<gpu::DeviceLostState> ConsumeDeviceLostCallbackState(void* userdata) {
  auto* token = static_cast<DeviceLostCallbackToken*>(userdata);
  DeviceLostCallbackStatus expected = DeviceLostCallbackStatus::Pending;
  if (!token->status.compare_exchange_strong(expected, DeviceLostCallbackStatus::Running,
                                             std::memory_order_acq_rel)) {
    return {};
  }

  std::shared_ptr<gpu::DeviceLostState> state = token->state;
  token->status.store(DeviceLostCallbackStatus::Done, std::memory_order_release);
  ReleaseDeviceLostCallbackTokenReference(token);
  return state;
}

void ReleaseDeviceLostCallbackToken(void*& userdata, bool callbackCannotRun) {
  if (userdata == nullptr) {
    return;
  }

  auto* token = static_cast<DeviceLostCallbackToken*>(userdata);
  if (callbackCannotRun) {
    DeviceLostCallbackStatus expected = DeviceLostCallbackStatus::Pending;
    if (token->status.compare_exchange_strong(expected, DeviceLostCallbackStatus::Canceled,
                                              std::memory_order_acq_rel)) {
      ReleaseDeviceLostCallbackTokenReference(token);
    }
  }

  ReleaseDeviceLostCallbackTokenReference(token);
  userdata = nullptr;
}

/// Error callback wired onto the backend device. Any driver-level validation errors (missing
/// bindings, bad draw parameters, and the rest) surface here.
///
/// The WebGPU C API passes the message as a `WGPUStringView` (pointer + length) rather than a
/// NUL-terminated string, so the precision-length form of `printf` is what avoids reading past
/// `length`.
void OnUncapturedError(WGPUDevice const* /*device*/, WGPUErrorType type, WGPUStringView message,
                       void* /*userdata1*/, void* /*userdata2*/) {
  std::fprintf(stderr, "[Geode/wgpu-native] Uncaptured error (type=%d): %.*s\n",
               static_cast<int>(type), static_cast<int>(message.length),
               message.data ? message.data : "");
}

/// Device-lost callback wired onto a selected device. Fires at most once per device; `userdata1`
/// carries a retained reference to the root's loss condition so a spontaneous callback can
/// outlive everything that registered it. A driver-reported loss sets the same condition that
/// bounded-wait timeouts set, so both failure modes surface as one detectable state.
void OnDeviceLost(WGPUDevice const* /*device*/, WGPUDeviceLostReason reason, WGPUStringView message,
                  void* userdata1, void* /*userdata2*/) {
  const std::shared_ptr<gpu::DeviceLostState> state = ConsumeDeviceLostCallbackState(userdata1);
  if (reason == WGPUDeviceLostReason_Destroyed || reason == WGPUDeviceLostReason_InstanceDropped) {
    // Expected teardown paths, not a driver failure.
    return;
  }
  std::fprintf(stderr, "[Geode/wgpu-native] Device lost (reason=%d): %.*s\n",
               static_cast<int>(reason), static_cast<int>(message.length),
               message.data ? message.data : "");
  if (state) {
    // Route through the shared declarer rather than storing the flag: it is what leaves
    // `timedOutSite` empty, which is how a report tells a driver-reported loss from one a bounded
    // wait's deadline discovered.
    gpu::DeclareDeviceLost(*state);
  }
}

wgpu::BackendType RequestedBackend() {
  const char* backendEnv = std::getenv("WGPU_BACKEND");
  if (backendEnv != nullptr && backendEnv[0] != '\0') {
    const std::string_view backend(backendEnv);
    using namespace std::string_view_literals;

    if (StringUtils::EqualsLowercase(backend, "vulkan"sv)) {
      return wgpu::BackendType::Vulkan;
    }
    if (StringUtils::EqualsLowercase(backend, "metal"sv)) {
      return wgpu::BackendType::Metal;
    }
    if (StringUtils::EqualsLowercase(backend, "opengl"sv) ||
        StringUtils::EqualsLowercase(backend, "gl"sv)) {
      return wgpu::BackendType::OpenGL;
    }
    if (StringUtils::EqualsLowercase(backend, "opengles"sv) ||
        StringUtils::EqualsLowercase(backend, "gles"sv)) {
      return wgpu::BackendType::OpenGLES;
    }

    std::fprintf(stderr, "[Geode/wgpu-native] Ignoring unsupported WGPU_BACKEND=%.*s.\n",
                 static_cast<int>(backend.size()), backend.data());
  }

  return wgpu::BackendType::Vulkan;
}

WGPUInstanceBackend InstanceBackendsFor(wgpu::BackendType backendType) {
  switch (static_cast<WGPUBackendType>(backendType)) {
    case WGPUBackendType_Vulkan: return WGPUInstanceBackend_Vulkan;
    case WGPUBackendType_Metal: return WGPUInstanceBackend_Metal;
    case WGPUBackendType_OpenGL:
    case WGPUBackendType_OpenGLES: return WGPUInstanceBackend_GL;
    case WGPUBackendType_D3D12: return WGPUInstanceBackend_DX12;
    case WGPUBackendType_D3D11: return WGPUInstanceBackend_DX11;
    case WGPUBackendType_WebGPU: return WGPUInstanceBackend_BrowserWebGPU;
    default: return WGPUInstanceBackend_All;
  }
}

wgpu::Instance CreateSelectionInstance(wgpu::BackendType backendType) {
  const WGPUInstanceBackend instanceBackends = InstanceBackendsFor(backendType);
  wgpu::Instance instance;
  if (instanceBackends != WGPUInstanceBackend_All) {
    wgpu::InstanceExtras instanceExtras = wgpu::Default;
    instanceExtras.backends = instanceBackends;

    wgpu::InstanceDescriptor instanceDesc = wgpu::Default;
    instanceDesc.nextInChain = &instanceExtras.chain;
    instance = wgpu::createInstance(instanceDesc);
  } else {
    instance = wgpu::createInstance();
  }
  return instance;
}

/// Backoff schedule shared by the adapter and device requests below. Under heavy parallel load
/// both fail transiently at the driver level - wgpu-native reports a validation error and returns
/// null - and succeed on a re-request after a short pause. Every retry is logged so the flake
/// stays observable rather than silently absorbed.
constexpr int kRequestBackoffMs[] = {50, 200, 800};

/// Whether a failed backend request should be retried, sleeping for its backoff first.
/// @param attempt Zero-based attempt that just failed.
/// @param what Name of the request, for the log line.
/// @return True when the caller should re-request; false once the schedule is exhausted.
bool RetryBackendRequest(int attempt, const char* what) {
  if (attempt >= static_cast<int>(std::size(kRequestBackoffMs))) {
    std::fprintf(stderr, "[Geode/wgpu-native] Giving up after %zu %s retries.\n",
                 std::size(kRequestBackoffMs), what);
    return false;
  }
  const int backoffMs = kRequestBackoffMs[attempt];
  std::fprintf(stderr,
               "[Geode/wgpu-native] Transient %s failure under parallel load; retrying "
               "(attempt %d of %zu) after %d ms.\n",
               what, attempt + 1, std::size(kRequestBackoffMs), backoffMs);
  std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
  return true;
}

/// Name of a wgpu-native backend type for the adapter selection log.
/// @param backendType Backend reported by `wgpuAdapterGetInfo`.
/// @return Display name, or "?" for a value this file does not recognize.
std::string_view BackendTypeName(WGPUBackendType backendType) {
  switch (backendType) {
    case WGPUBackendType_Vulkan: return "Vulkan";
    case WGPUBackendType_Metal: return "Metal";
    case WGPUBackendType_D3D12: return "D3D12";
    case WGPUBackendType_D3D11: return "D3D11";
    case WGPUBackendType_OpenGL: return "OpenGL";
    case WGPUBackendType_OpenGLES: return "OpenGLES";
    case WGPUBackendType_WebGPU: return "WebGPU";
    case WGPUBackendType_Null: return "Null";
    default: return "?";
  }
}

/// Name of a wgpu-native adapter type for the adapter selection log.
/// @param adapterType Adapter class reported by `wgpuAdapterGetInfo`.
/// @return Display name, or "?" for a value this file does not recognize.
std::string_view AdapterTypeName(WGPUAdapterType adapterType) {
  switch (adapterType) {
    case WGPUAdapterType_DiscreteGPU: return "DiscreteGPU";
    case WGPUAdapterType_IntegratedGPU: return "IntegratedGPU";
    case WGPUAdapterType_CPU: return "CPU";
    case WGPUAdapterType_Unknown: return "Unknown";
    default: return "?";
  }
}

/// Logs which adapter the selection landed on and reports whether it is a Vulkan backend.
///
/// The log makes it obvious at a glance whether the process is on a discrete GPU, an integrated
/// GPU or a software rasterizer, and which native backend is driving it. If the query fails on a
/// real Vulkan device the filter-engine serialization silently disables, which is accepted
/// residual risk rather than a reason to fail the selection.
/// @param adapter Adapter to describe.
/// @return Whether the adapter reports a Vulkan backend; false when the query failed.
bool DescribeSelectedAdapter(const wgpu::Adapter& adapter) {
  WGPUAdapterInfo info = {};
  if (wgpuAdapterGetInfo(adapter, &info) != WGPUStatus_Success) {
    return false;
  }
  const auto text = [](const WGPUStringView& value) {
    return std::string_view{value.data ? value.data : "", value.data ? value.length : 0};
  };
  const std::string_view backend = BackendTypeName(info.backendType);
  const std::string_view type = AdapterTypeName(info.adapterType);
  const auto vendor = text(info.vendor);
  const auto device = text(info.device);
  const auto architecture = text(info.architecture);
  std::fprintf(stderr,
               "[Geode/wgpu-native] Adapter: %.*s %.*s (%.*s) "
               "backend=%.*s type=%.*s vendorID=0x%04x deviceID=0x%04x\n",
               static_cast<int>(vendor.size()), vendor.data(), static_cast<int>(device.size()),
               device.data(), static_cast<int>(architecture.size()), architecture.data(),
               static_cast<int>(backend.size()), backend.data(), static_cast<int>(type.size()),
               type.data(), info.vendorID, info.deviceID);
  const bool isVulkan = info.backendType == WGPUBackendType_Vulkan;
  wgpuAdapterInfoFreeMembers(info);
  return isVulkan;
}

/// Queries what every runtime device over one wgpu device will answer identically.
/// @param device Device whose limits to read.
/// @param adapter Adapter the device came from, described once in the log.
GeodeGpuRootCapabilities QueryRootCapabilities(const wgpu::Device& device,
                                               const wgpu::Adapter& adapter) {
  GeodeGpuRootCapabilities capabilities;
  wgpu::Limits limits;
  if (device.getLimits(&limits) == wgpu::Status::Success &&
      limits.maxTextureDimension2D != WGPU_LIMIT_U32_UNDEFINED &&
      limits.maxTextureDimension2D > 0) {
    capabilities.maxTextureDimension2D = limits.maxTextureDimension2D;
  }
  if (adapter) {
    capabilities.isVulkan = DescribeSelectedAdapter(adapter);
  }
  return capabilities;
}

void ReleaseSelectedHandles(GeodeWgpuRoots& handles) {
  ReleaseWgpuHandle(handles.queue);
  if (handles.device) {
    handles.device.destroy();
  }
  ReleaseWgpuHandle(handles.device);
  ReleaseWgpuHandle(handles.adapter);
  ReleaseWgpuHandle(handles.instance);
  ReleaseDeviceLostCallbackToken(handles.deviceLostCallbackToken, /*callbackCannotRun=*/true);
}

/// Selects the wgpu objects the reference renders through: creates an instance, requests an
/// adapter and a device, and takes the default queue.
/// @param label Label the selected device carries in driver diagnostics.
/// @param lostState Loss condition the device-lost callback publishes into.
/// @return The selected objects, or null when no adapter or device could be obtained.
std::shared_ptr<WgpuReferenceRoot> SelectWgpuReferenceRoot(
    std::string_view label, std::shared_ptr<gpu::DeviceLostState> lostState) {
  GeodeWgpuRoots handles;
  PartialSelection partial(handles);
  // 1. Create the instance. `wgpuCreateInstance` is synchronous and never blocks on I/O; the
  //    returned handle is the root of the object graph.
  const wgpu::BackendType backendType = RequestedBackend();
  handles.instance = CreateSelectionInstance(backendType);
  if (!handles.instance) {
    std::fprintf(stderr, "[Geode/wgpu-native] wgpuCreateInstance returned null\n");
    return nullptr;
  }
  // 2. Request an adapter. The synchronous form in webgpu-cpp internally calls the async C API
  //    with a lambda that parks the result on the stack - wgpu-native invokes the callback before
  //    returning from the request, so the sync form is safe on native targets.
  wgpu::RequestAdapterOptions adapterOptions = {};
  adapterOptions.backendType = backendType;
  adapterOptions.forceFallbackAdapter = wgpuForceFallbackAdapterRequested();

  for (int attempt = 0;; ++attempt) {
    handles.adapter = handles.instance.requestAdapter(adapterOptions);
    if (handles.adapter) {
      break;
    }
    std::fprintf(stderr, "[Geode/wgpu-native] No WebGPU adapter available.\n");
    if (!RetryBackendRequest(attempt, "adapter-acquisition")) {
      return nullptr;
    }
  }

  // 3. Create the device. Error diagnostics are wired through the descriptor; the callbacks stay
  //    valid for the device's lifetime.
  wgpu::DeviceDescriptor deviceDesc = {};
  deviceDesc.label = wgpu::StringView{label};
  deviceDesc.uncapturedErrorCallbackInfo.callback = OnUncapturedError;
  deviceDesc.uncapturedErrorCallbackInfo.userdata1 = nullptr;
  deviceDesc.uncapturedErrorCallbackInfo.userdata2 = nullptr;

  // Only a null device return is retried here. The deterministic failures (null instance, no
  // adapter for the requested backend) already returned above, and a device lost after successful
  // creation is out of scope.
  for (int attempt = 0;; ++attempt) {
    // A fresh retained loss-condition reference per attempt: each successfully created device
    // eventually consumes its userdata exactly once through OnDeviceLost, including the
    // Destroyed-at-teardown delivery. An attempt that returns a null device strands at most one
    // small retained block, bounded by the retry count.
    handles.deviceLostCallbackToken = CreateDeviceLostCallbackToken(lostState);
    deviceDesc.deviceLostCallbackInfo.mode = wgpu::CallbackMode::AllowSpontaneous;
    deviceDesc.deviceLostCallbackInfo.callback = OnDeviceLost;
    deviceDesc.deviceLostCallbackInfo.userdata1 = handles.deviceLostCallbackToken;
    deviceDesc.deviceLostCallbackInfo.userdata2 = nullptr;

    handles.device = handles.adapter.requestDevice(deviceDesc);
    if (handles.device) {
      break;
    }
    ReleaseDeviceLostCallbackToken(handles.deviceLostCallbackToken, /*callbackCannotRun=*/true);

    std::fprintf(stderr, "[Geode/wgpu-native] Failed to create device.\n");
    if (!RetryBackendRequest(attempt, "device-creation")) {
      return nullptr;
    }
  }

  // 4. Grab the default queue.
  handles.queue = handles.device.getQueue();
  if (!handles.queue) {
    std::fprintf(stderr, "[Geode/wgpu-native] Failed to get queue.\n");
    return nullptr;
  }

  partial.keep();
  return std::make_shared<WgpuReferenceRoot>(std::move(handles), std::move(lostState));
}

/// Opens wgpu reference runtime devices over one selected set of wgpu objects.
class WgpuReferenceDeviceSource final : public GeodeRuntimeDeviceSource {
public:
  /// @param root wgpu objects every opened device records against; must not be null.
  explicit WgpuReferenceDeviceSource(std::shared_ptr<const WgpuReferenceRoot> root)
      : root_(std::move(root)) {}

  std::unique_ptr<gpu::Device> openRuntimeDevice() override {
    return std::make_unique<GeodeWgpuAdapterDevice>(root_);
  }

private:
  std::shared_ptr<const WgpuReferenceRoot> root_;
};

}  // namespace

WgpuReferenceRoot::WgpuReferenceRoot(GeodeWgpuRoots handles,
                                     std::shared_ptr<gpu::DeviceLostState> lostState)
    : handles_(std::move(handles)), lostState_(std::move(lostState)) {
  UTILS_RELEASE_ASSERT(handles_.device && handles_.queue && lostState_ != nullptr);
}

WgpuReferenceRoot::~WgpuReferenceRoot() {
  if (lostState_->lost.load(std::memory_order_acquire)) {
    // A lost device is a process-fatal condition for GPU rendering, and releasing it calls into a
    // driver that has stopped answering. Leak one root's worth of driver objects rather than risk
    // a blocking call into a hung driver.
    ReleaseDeviceLostCallbackToken(handles_.deviceLostCallbackToken, /*callbackCannotRun=*/false);
    return;
  }
  ReleaseSelectedHandles(handles_);
}

WgpuReferenceSelection SelectWgpuReference(std::string_view label) {
  auto lostState = std::make_shared<gpu::DeviceLostState>();
  std::shared_ptr<WgpuReferenceRoot> reference = SelectWgpuReferenceRoot(label, lostState);
  if (reference == nullptr) {
    return {};
  }
  std::shared_ptr<GeodeGpuRoot> root = AdoptRuntimeDeviceSource(
      std::make_shared<WgpuReferenceDeviceSource>(reference),
      QueryRootCapabilities(reference->device(), reference->adapter()), std::move(lostState));
  if (root == nullptr) {
    return {};
  }
  return WgpuReferenceSelection{.root = std::move(root), .reference = std::move(reference)};
}

std::unique_ptr<GeodeDevice> CreateWgpuReferenceContext(gpu::TextureFormat textureFormat) {
  WgpuReferenceSelection selection = SelectWgpuReference("GeodeWgpuReference");
  if (selection.root == nullptr) {
    return nullptr;
  }
  return GeodeDevice::CreateOverSelectedRoot(std::move(selection.root), textureFormat);
}

namespace {

using gpu::GpuError;
using gpu::GpuErrorType;
using gpu::OkStatus;

/// Makes a caller's minimally-sized final row safe for wgpu-native implementations that read a
/// complete final row pitch. Keeps already-complete spans zero-copy and otherwise repacks into the
/// smallest aligned row pitch, independent of unused stride in the caller's layout.
gpu::Status PrepareWgpuTextureUpload(wgpu::TextureFormat textureFormat,
                                     std::span<const uint8_t> callerData,
                                     const gpu::TexelCopyBufferLayout& callerLayout,
                                     const gpu::Extent2d& writeSize,
                                     SmallVector<uint8_t, gpu::kTexelRowPitchAlignment>& ownedData,
                                     std::span<const uint8_t>& uploadData,
                                     gpu::TexelCopyBufferLayout& uploadLayout) {
  uploadData = callerData;
  uploadLayout = callerLayout;
  const std::optional<uint64_t> fullRows =
      gpu::CheckedMul(writeSize.height, callerLayout.bytesPerRow);
  const std::optional<uint64_t> fullEnd =
      fullRows ? gpu::CheckedAdd(callerLayout.offsetBytes, *fullRows) : std::nullopt;
  if (fullEnd && *fullEnd <= callerData.size()) {
    return OkStatus();
  }

  const uint32_t texelBytes =
      gpu::TextureFormatBytesPerTexel(GpuTextureFormatFromWgpu(textureFormat));
  const std::optional<uint64_t> rowBytes = gpu::CheckedMul(writeSize.width, texelBytes);
  const std::optional<uint64_t> roundedRowBytes =
      rowBytes ? gpu::CheckedAdd(*rowBytes, gpu::kTexelRowPitchAlignment - 1) : std::nullopt;
  if (!roundedRowBytes) {
    return GpuError{GpuErrorType::OutOfBounds,
                    "writeTexture: compact wgpu row byte size overflows"};
  }
  const uint64_t compactBytesPerRow =
      (*roundedRowBytes / gpu::kTexelRowPitchAlignment) * gpu::kTexelRowPitchAlignment;
  const std::optional<uint64_t> compactBytes =
      gpu::CheckedMul(compactBytesPerRow, writeSize.height);
  if (!compactBytes || *compactBytes > std::numeric_limits<size_t>::max()) {
    return GpuError{GpuErrorType::OutOfBounds,
                    "writeTexture: compact wgpu upload byte size overflows"};
  }

  ownedData.resize(static_cast<size_t>(*compactBytes));
  for (uint32_t row = 0; row < writeSize.height; ++row) {
    const std::optional<uint64_t> sourceRowOffset =
        gpu::CheckedMul(static_cast<uint64_t>(row), callerLayout.bytesPerRow);
    const std::optional<uint64_t> sourceRow =
        sourceRowOffset ? gpu::CheckedAdd(callerLayout.offsetBytes, *sourceRowOffset)
                        : std::nullopt;
    const std::optional<uint64_t> sourceEnd =
        sourceRow ? gpu::CheckedAdd(*sourceRow, *rowBytes) : std::nullopt;
    if (!sourceEnd || *sourceEnd > callerData.size()) {
      return GpuError{GpuErrorType::OutOfBounds,
                      "writeTexture: validated caller row range became invalid"};
    }
    std::copy_n(
        callerData.begin() + static_cast<size_t>(*sourceRow), static_cast<size_t>(*rowBytes),
        ownedData.begin() + static_cast<size_t>(row) * static_cast<size_t>(compactBytesPerRow));
  }
  uploadData = std::span<const uint8_t>(ownedData.data(), ownedData.size());
  uploadLayout = {0, static_cast<uint32_t>(compactBytesPerRow), writeSize.height};
  return OkStatus();
}

/// Ensures \p table covers \p slotIndex and stores \p value there. Slots are value-initialized
/// (null handles) until written.
template <typename T>
void SetSlot(std::vector<T>& table, uint32_t slotIndex, T value) {
  if (table.size() <= slotIndex) {
    table.resize(slotIndex + 1);
  }
  table[slotIndex] = std::move(value);
}

/// Returns a borrowed alias of the wgpu handle at \p slotIndex, or a null handle if the slot is
/// out of range or dead.
template <typename Handle>
Handle GetHandle(const std::vector<ScopedWgpuHandle<Handle>>& table, uint32_t slotIndex) {
  return slotIndex < table.size() ? table[slotIndex].get() : Handle{};
}

/// Overload set for exhaustive std::visit dispatch: adding a new `gpu::Command` alternative
/// without a matching handler is a compile error instead of a silently dropped command.
template <typename... Ts>
struct Overloaded : Ts... {
  using Ts::operator()...;
};
template <typename... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

wgpu::BufferUsage ToWgpuBufferUsage(gpu::BufferUsage usage) {
  WGPUBufferUsage result = wgpu::BufferUsage::None;
  if (gpu::HasAllFlags(usage, gpu::BufferUsage::Vertex)) {
    result |= wgpu::BufferUsage::Vertex;
  }
  if (gpu::HasAllFlags(usage, gpu::BufferUsage::Index)) {
    result |= wgpu::BufferUsage::Index;
  }
  if (gpu::HasAllFlags(usage, gpu::BufferUsage::Uniform)) {
    result |= wgpu::BufferUsage::Uniform;
  }
  if (gpu::HasAllFlags(usage, gpu::BufferUsage::Storage)) {
    result |= wgpu::BufferUsage::Storage;
  }
  if (gpu::HasAllFlags(usage, gpu::BufferUsage::CopySrc)) {
    result |= wgpu::BufferUsage::CopySrc;
  }
  if (gpu::HasAllFlags(usage, gpu::BufferUsage::CopyDst)) {
    result |= wgpu::BufferUsage::CopyDst;
  }
  if (gpu::HasAllFlags(usage, gpu::BufferUsage::MapRead)) {
    result |= wgpu::BufferUsage::MapRead;
  }
  return result;
}

wgpu::TextureUsage ToWgpuTextureUsage(gpu::TextureUsage usage) {
  WGPUTextureUsage result = wgpu::TextureUsage::None;
  if (gpu::HasAllFlags(usage, gpu::TextureUsage::RenderAttachment)) {
    result |= wgpu::TextureUsage::RenderAttachment;
  }
  if (gpu::HasAllFlags(usage, gpu::TextureUsage::Sampled)) {
    result |= wgpu::TextureUsage::TextureBinding;
  }
  if (gpu::HasAllFlags(usage, gpu::TextureUsage::CopySrc)) {
    result |= wgpu::TextureUsage::CopySrc;
  }
  if (gpu::HasAllFlags(usage, gpu::TextureUsage::CopyDst)) {
    result |= wgpu::TextureUsage::CopyDst;
  }
  if (gpu::HasAllFlags(usage, gpu::TextureUsage::StorageBinding)) {
    result |= wgpu::TextureUsage::StorageBinding;
  }
  return result;
}

wgpu::ShaderStage ToWgpuShaderStage(gpu::ShaderStage visibility) {
  WGPUShaderStage result = wgpu::ShaderStage::None;
  if (gpu::HasAllFlags(visibility, gpu::ShaderStage::Vertex)) {
    result |= wgpu::ShaderStage::Vertex;
  }
  if (gpu::HasAllFlags(visibility, gpu::ShaderStage::Fragment)) {
    result |= wgpu::ShaderStage::Fragment;
  }
  if (gpu::HasAllFlags(visibility, gpu::ShaderStage::Compute)) {
    result |= wgpu::ShaderStage::Compute;
  }
  return result;
}

wgpu::ColorWriteMask ToWgpuColorWriteMask(gpu::ColorWriteMask mask) {
  WGPUColorWriteMask result = wgpu::ColorWriteMask::None;
  if (gpu::HasAllFlags(mask, gpu::ColorWriteMask::Red)) {
    result |= wgpu::ColorWriteMask::Red;
  }
  if (gpu::HasAllFlags(mask, gpu::ColorWriteMask::Green)) {
    result |= wgpu::ColorWriteMask::Green;
  }
  if (gpu::HasAllFlags(mask, gpu::ColorWriteMask::Blue)) {
    result |= wgpu::ColorWriteMask::Blue;
  }
  if (gpu::HasAllFlags(mask, gpu::ColorWriteMask::Alpha)) {
    result |= wgpu::ColorWriteMask::Alpha;
  }
  return result;
}

wgpu::FilterMode ToWgpuFilterMode(gpu::FilterMode mode) {
  switch (mode) {
    case gpu::FilterMode::Nearest: return wgpu::FilterMode::Nearest;
    case gpu::FilterMode::Linear: return wgpu::FilterMode::Linear;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated FilterMode out of range");
  return wgpu::FilterMode::Nearest;
}

wgpu::AddressMode ToWgpuAddressMode(gpu::AddressMode mode) {
  switch (mode) {
    case gpu::AddressMode::ClampToEdge: return wgpu::AddressMode::ClampToEdge;
    case gpu::AddressMode::Repeat: return wgpu::AddressMode::Repeat;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated AddressMode out of range");
  return wgpu::AddressMode::ClampToEdge;
}

wgpu::VertexFormat ToWgpuVertexFormat(gpu::VertexFormat format) {
  switch (format) {
    case gpu::VertexFormat::Float32x2: return wgpu::VertexFormat::Float32x2;
    case gpu::VertexFormat::Float32x4: return wgpu::VertexFormat::Float32x4;
    case gpu::VertexFormat::Uint32: return wgpu::VertexFormat::Uint32;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated VertexFormat out of range");
  return wgpu::VertexFormat::Float32x2;
}

wgpu::VertexStepMode ToWgpuVertexStepMode(gpu::VertexStepMode mode) {
  switch (mode) {
    case gpu::VertexStepMode::Vertex: return wgpu::VertexStepMode::Vertex;
    case gpu::VertexStepMode::Instance: return wgpu::VertexStepMode::Instance;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated VertexStepMode out of range");
  return wgpu::VertexStepMode::Vertex;
}

wgpu::IndexFormat ToWgpuIndexFormat(gpu::IndexFormat format) {
  return format == gpu::IndexFormat::Uint16 ? wgpu::IndexFormat::Uint16 : wgpu::IndexFormat::Uint32;
}

wgpu::PrimitiveTopology ToWgpuPrimitiveTopology(gpu::PrimitiveTopology topology) {
  switch (topology) {
    case gpu::PrimitiveTopology::TriangleList: return wgpu::PrimitiveTopology::TriangleList;
    case gpu::PrimitiveTopology::TriangleStrip: return wgpu::PrimitiveTopology::TriangleStrip;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated PrimitiveTopology out of range");
  return wgpu::PrimitiveTopology::TriangleList;
}

wgpu::CullMode ToWgpuCullMode(gpu::CullMode mode) {
  switch (mode) {
    case gpu::CullMode::None: return wgpu::CullMode::None;
    case gpu::CullMode::Back: return wgpu::CullMode::Back;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated CullMode out of range");
  return wgpu::CullMode::None;
}

wgpu::BlendFactor ToWgpuBlendFactor(gpu::BlendFactor factor) {
  switch (factor) {
    case gpu::BlendFactor::Zero: return wgpu::BlendFactor::Zero;
    case gpu::BlendFactor::One: return wgpu::BlendFactor::One;
    case gpu::BlendFactor::SrcAlpha: return wgpu::BlendFactor::SrcAlpha;
    case gpu::BlendFactor::OneMinusSrcAlpha: return wgpu::BlendFactor::OneMinusSrcAlpha;
    case gpu::BlendFactor::OneMinusDstAlpha: return wgpu::BlendFactor::OneMinusDstAlpha;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated BlendFactor out of range");
  return wgpu::BlendFactor::Zero;
}

wgpu::BlendOperation ToWgpuBlendOperation(gpu::BlendOperation operation) {
  switch (operation) {
    case gpu::BlendOperation::Add: return wgpu::BlendOperation::Add;
    case gpu::BlendOperation::Max: return wgpu::BlendOperation::Max;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated BlendOperation out of range");
  return wgpu::BlendOperation::Add;
}

wgpu::LoadOp ToWgpuLoadOp(gpu::LoadOp op) {
  switch (op) {
    case gpu::LoadOp::Clear: return wgpu::LoadOp::Clear;
    case gpu::LoadOp::Load: return wgpu::LoadOp::Load;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated LoadOp out of range");
  return wgpu::LoadOp::Clear;
}

wgpu::StoreOp ToWgpuStoreOp(gpu::StoreOp op) {
  switch (op) {
    case gpu::StoreOp::Store: return wgpu::StoreOp::Store;
    case gpu::StoreOp::Discard: return wgpu::StoreOp::Discard;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated StoreOp out of range");
  return wgpu::StoreOp::Store;
}

/// Fills the resource-kind union of a wgpu bind group layout entry from \p layoutEntry.
void ApplyBindingType(wgpu::BindGroupLayoutEntry& entry,
                      const gpu::BindGroupLayoutEntry& layoutEntry) {
  switch (layoutEntry.type) {
    case gpu::BindingType::UniformBuffer:
      entry.buffer.type = wgpu::BufferBindingType::Uniform;
      entry.buffer.minBindingSize = 0;
      return;
    case gpu::BindingType::ReadOnlyStorageBuffer:
      entry.buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
      entry.buffer.minBindingSize = 0;
      return;
    case gpu::BindingType::SampledTexture2dFloat:
      entry.texture.sampleType = wgpu::TextureSampleType::Float;
      entry.texture.viewDimension = wgpu::TextureViewDimension::_2D;
      entry.texture.multisampled = false;
      return;
    case gpu::BindingType::SampledTexture2dUnfilterableFloat:
      entry.texture.sampleType = wgpu::TextureSampleType::UnfilterableFloat;
      entry.texture.viewDimension = wgpu::TextureViewDimension::_2D;
      entry.texture.multisampled = false;
      return;
    case gpu::BindingType::FilteringSampler:
      entry.sampler.type = wgpu::SamplerBindingType::Filtering;
      return;
    case gpu::BindingType::WriteOnlyStorageTexture2d:
      entry.storageTexture.access = wgpu::StorageTextureAccess::WriteOnly;
      entry.storageTexture.format = WgpuTextureFormatFrom(layoutEntry.storageTextureFormat);
      entry.storageTexture.viewDimension = wgpu::TextureViewDimension::_2D;
      return;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated BindingType out of range");
}

}  // namespace

GeodeWgpuAdapterDevice::GeodeWgpuAdapterDevice(std::shared_ptr<const WgpuReferenceRoot> root)
    : root_(std::move(root)) {
  UTILS_RELEASE_ASSERT(root_ != nullptr);
  // Every runtime device over one backend root answers the same question about whether that root
  // has stopped answering, so take the root's condition rather than minting a private one.
  adoptLostState(root_->lostState());
}

GeodeWgpuAdapterDevice::~GeodeWgpuAdapterDevice() {
  // Wait for in-flight submissions so deferred destructions drain before the slot vectors
  // release the remaining wgpu objects. On timeout teardown proceeds anyway: wgpu retains every
  // resource referenced by a submitted command buffer until it completes. That tolerance is why
  // the drain declares nothing - it is nobody's deadline, it overruns on a loaded host, and the
  // other contexts over this root are still rendering through it.
  if (lastSubmittedSerial() > completedSerial()) {
    waitForSerialBounded(lastSubmittedSerial(), kTeardownDrainSeconds, LossOnTimeout::Tolerate);
  }
  poll();
}

uint64_t GeodeWgpuAdapterDevice::completedSerial() const {
  // The ceiling is `kNoCompletedSerialCeiling` outside the tests that hold submitted work
  // incomplete, so this is the backend's own answer everywhere else.
  return std::min(completionState_->completedSerial.load(std::memory_order_acquire),
                  completedSerialCeiling_.load(std::memory_order_relaxed));
}

void GeodeWgpuAdapterDevice::onPollBackend() {
  root_->device().poll(false, nullptr);
}

bool GeodeWgpuAdapterDevice::onWaitForSerial(uint64_t serial, double timeoutSeconds) {
  return waitForSerialBounded(serial, timeoutSeconds, LossOnTimeout::Declare);
}

bool GeodeWgpuAdapterDevice::waitForSerialBounded(uint64_t serial, double timeoutSeconds,
                                                  LossOnTimeout onTimeout) {
  const auto start = std::chrono::steady_clock::now();
  const auto deadline = start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                    std::chrono::duration<double>(timeoutSeconds));
  CompletionState& state = *completionState_;
  for (;;) {
    if (completedSerial() >= serial) {
      return true;
    }
    if (isLost()) {
      // Nothing will complete on a lost device, so the budget is not spent waiting for something
      // that can never arrive - and polling a lost wgpu device is what hangs on some drivers.
      return false;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return giveUpOnSerialWait(start, timeoutSeconds, onTimeout);
    }
    // Never a blocking poll: one that waits for pending work cannot be interrupted at the
    // deadline, so a stalled submission would hold the caller in the driver indefinitely. A
    // nonblocking poll runs whatever completion callbacks are ready, outside the lock because
    // they complete through it, and the rest of the slice waits for a completion another
    // thread's poll delivers.
    root_->device().poll(false, nullptr);
    std::unique_lock lock(state.mutex);
    state.progressed.wait_until(
        lock, std::min(deadline, std::chrono::steady_clock::now() + kSerialWaitSlice),
        [&] { return completedSerial() >= serial; });
  }
}

bool GeodeWgpuAdapterDevice::giveUpOnSerialWait(std::chrono::steady_clock::time_point start,
                                                double timeoutSeconds, LossOnTimeout onTimeout) {
  // A budget of zero is a question about what is already known rather than a wait, so its
  // negative answer is no evidence that the device stopped answering.
  if (onTimeout == LossOnTimeout::Declare && timeoutSeconds > 0.0) {
    // Report the wait that actually ran, not the budget it was given: the budget is a constant
    // the reader already knows, while the measurement says whether the wait gave up on schedule
    // or overran under load.
    markLostAfterWaitTimeout(gpu::DeviceLostWaitSite::QueueIdle,
                             std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - start),
                             "submitted GPU work did not complete within the bounded wait");
  }
  return false;
}

bool GeodeWgpuAdapterDevice::onOwnsTextureBacking(uint32_t slotIndex) const {
  return slotIndex < slotTextures_.size() &&
         static_cast<bool>(slotTextures_[slotIndex].ownedTexture);
}

void GeodeWgpuAdapterDevice::onDestroyTextureBacking(uint32_t slotIndex) {
  if (slotIndex < slotTextures_.size()) {
    // A registration of a sibling's export leaves `ownedTexture` empty, so this destroys only
    // what this adapter allocated; the sibling's texture is left to the sibling.
    slotTextures_[slotIndex].ownedTexture.destroyBackingAndReset();
  }
}

void GeodeWgpuAdapterDevice::onDestroyBufferBacking(uint32_t slotIndex) {
  if (slotIndex < slotBuffers_.size()) {
    slotBuffers_[slotIndex].destroyBackingAndReset();
  }
}

namespace {

/// Address that identifies this adapter in a \ref gpu::BackendDeviceIdentity. Its value differs
/// from every other backend's tag so no constant merging can give two backends one address.
constexpr char kWgpuTextureShareFamily = 'W';

/// A wgpu texture exported to a sibling adapter over the same root. It holds a reference of its
/// own on the texture, and on the root so the wgpu device outlives that reference, because the
/// last registration may be released after the exporting adapter is gone.
class WgpuExportedTexture final : public gpu::ExportedTextureBacking {
public:
  WgpuExportedTexture(wgpu::Texture texture, std::shared_ptr<const WgpuReferenceRoot> root)
      : root_(std::move(root)), texture_(AddedReference(std::move(texture))) {}

  /// Destroys the texture's backing, for an owner that released its backing while a sibling still
  /// read it; the sibling has let go by the time this runs.
  void releaseBackingNow() const override { texture_.destroyBackingAndReset(); }

  /// The exported texture; borrowed, this object holds the reference.
  const wgpu::Texture& texture() const UTILS_LIFETIME_BOUND { return texture_.get(); }
  /// The root the exporting adapter records against.
  const WgpuReferenceRoot& root() const UTILS_LIFETIME_BOUND { return *root_; }

private:
  /// \p texture with a reference of its own taken. @param texture Texture to reference.
  static wgpu::Texture AddedReference(wgpu::Texture texture) {
    texture.addRef();
    return texture;
  }

  std::shared_ptr<const WgpuReferenceRoot> root_;
  /// Mutable because the release above runs through the const handle every holder shares, once,
  /// after every other holder is gone.
  mutable ScopedWgpuHandle<wgpu::Texture> texture_;
};

}  // namespace

gpu::BackendDeviceIdentity GeodeWgpuAdapterDevice::backendDeviceIdentity() const {
  return {&kWgpuTextureShareFamily, static_cast<WGPUDevice>(root_->device())};
}

gpu::Result<gpu::BackendTextureExport> GeodeWgpuAdapterDevice::onExportTexture(uint32_t slotIndex) {
  const wgpu::Texture texture =
      slotIndex < slotTextures_.size() ? slotTextures_[slotIndex].texture : wgpu::Texture();
  if (!texture) {
    return GpuError{GpuErrorType::InvalidHandle,
                    "exportTexture: the adapter has no backend texture for this handle"};
  }
  gpu::BackendTextureExport exported;
  exported.backing = std::make_shared<const WgpuExportedTexture>(texture, root_);
  exported.ordering = gpu::SourceOrdering::SharedQueue;
  return exported;
}

gpu::Status GeodeWgpuAdapterDevice::onRegisterTexture(uint32_t slotIndex,
                                                      const gpu::ExportedTextureBacking& backing) {
  const auto& exported = static_cast<const WgpuExportedTexture&>(backing);
  if (static_cast<WGPUQueue>(exported.root().queue()) != static_cast<WGPUQueue>(root_->queue())) {
    return GpuError{GpuErrorType::DeviceMismatch,
                    "registerTexture: the owning adapter submits to a different queue"};
  }
  // A registration names the texture without allocating it, so nothing is counted, and the empty
  // owned handle is what makes onOwnsTextureBacking report it as borrowed.
  SetSlot(slotTextures_, slotIndex,
          TextureSlot{ScopedWgpuHandle<wgpu::Texture>(), exported.texture()});
  return OkStatus();
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
  UTILS_RELEASE_ASSERT_MSG(false,
                           "wgpu texture format is outside the donner::gpu supported set "
                           "(RGBA8Unorm / BGRA8Unorm / R8Unorm / RGBA32Float)");
  return gpu::TextureFormat::RGBA8Unorm;
}

gpu::Status GeodeWgpuAdapterDevice::onMapBufferAsync(uint32_t mappingSlotIndex,
                                                     uint32_t bufferSlotIndex, gpu::MapMode mode,
                                                     uint64_t offsetBytes, uint64_t byteCount) {
  if (mode != gpu::MapMode::Read) {
    return GpuError{GpuErrorType::Unsupported, "this adapter maps buffers for reading only"};
  }
  wgpu::Buffer buffer = GetHandle(slotBuffers_, bufferSlotIndex);
  if (!buffer) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("buffer slot {} has no wgpu buffer", bufferSlotIndex)};
  }
  if (isLost()) {
    // A lost device never delivers the completion, so refuse the map rather than hand back a
    // handle whose wait can only ever run out its budget.
    return GpuError{GpuErrorType::InvalidState, "the device is lost, so buffers cannot be mapped"};
  }

  MappingSlot slot;
  slot.completion = new MappingSlot::Completion();
  // The completion keeps a reference of its own, so a map abandoned in flight still has a buffer
  // to unmap once it finishes.
  slot.completion->buffer = buffer;
  slot.completion->buffer.addRef();
  slot.buffer = buffer;
  slot.offsetBytes = offsetBytes;
  slot.byteCount = byteCount;

  wgpu::BufferMapCallbackInfo callbackInfo{wgpu::Default};
  callbackInfo.callback = [](WGPUMapAsyncStatus status, WGPUStringView /*message*/, void* userdata1,
                             void* /*userdata2*/) {
    auto* completion = static_cast<MappingSlot::Completion*>(userdata1);
    completion->ok.store(status == WGPUMapAsyncStatus_Success, std::memory_order_relaxed);
    completion->done.store(true, std::memory_order_release);
    // The mapping may already be gone, in which case nothing else can give the buffer back.
    completion->unmapIfAbandoned();
    completion->release();
  };
  callbackInfo.userdata1 = slot.completion;
  callbackInfo.userdata2 = nullptr;
  // Spontaneous delivery is what lets a wait slice observe the completion from inside the
  // backend call it makes, rather than only at an explicit processing point.
  callbackInfo.mode = wgpu::CallbackMode::AllowSpontaneous;
  slot.mapFuture = buffer.mapAsync(wgpu::MapMode::Read, offsetBytes, byteCount, callbackInfo);

  SetSlot(slotMappings_, mappingSlotIndex, std::move(slot));
  return OkStatus();
}

gpu::MapSliceState GeodeWgpuAdapterDevice::sliceStateOf(
    const MappingSlot::Completion& completion) const {
  // Total over the completion state on purpose. A wait that returns without the map having
  // completed - a timed wait that expired, a poll that found nothing - has learned nothing about
  // whether the map will succeed, and reading only the success flag there reports a map that is
  // merely not finished yet as a failed one. Every caller reads the state through here so that
  // mistake cannot be made at one site and not another.
  if (completion.done.load(std::memory_order_acquire)) {
    return completion.ok.load(std::memory_order_relaxed) ? gpu::MapSliceState::Ready
                                                         : gpu::MapSliceState::Failed;
  }
  return isLost() ? gpu::MapSliceState::DeviceLost : gpu::MapSliceState::Pending;
}

bool GeodeWgpuAdapterDevice::mappingStillMatches(uint32_t mappingSlotIndex,
                                                 const MappingSlot::Completion* completion,
                                                 wgpu::Future future) const {
  return mappingSlotIndex < slotMappings_.size() &&
         slotMappings_[mappingSlotIndex].completion == completion &&
         slotMappings_[mappingSlotIndex].mapFuture.id == future.id &&
         !completion->abandoned.load(std::memory_order_acquire);
}

gpu::MapSliceReport GeodeWgpuAdapterDevice::onWaitMappingSlice(uint32_t mappingSlotIndex,
                                                               double sliceSeconds) {
  if (mappingSlotIndex >= slotMappings_.size() ||
      slotMappings_[mappingSlotIndex].completion == nullptr) {
    return gpu::MapSliceReport{.state = gpu::MapSliceState::Failed,
                               .waitKind = gpu::MapWaitKind::Polled};
  }
  MappingSlot::Completion& completion = *slotMappings_[mappingSlotIndex].completion;
  if (completion.done.load(std::memory_order_acquire) || isLost()) {
    // Nothing was waited on, so no completion event was used to learn it.
    return gpu::MapSliceReport{.state = sliceStateOf(completion),
                               .waitKind = gpu::MapWaitKind::Polled};
  }

  // The polls below can run the completion callback or let an abandonment release the record, so
  // it is retained until the slice ends.
  completion.references.fetch_add(1, std::memory_order_relaxed);
  const auto releaseCompletion = [](MappingSlot::Completion* value) { value->release(); };
  const std::unique_ptr<MappingSlot::Completion, decltype(releaseCompletion)> retainedCompletion(
      &completion, releaseCompletion);
  const wgpu::Future future = slotMappings_[mappingSlotIndex].mapFuture;

  // Wait out the slice the caller allowed rather than returning the moment one poll finds
  // nothing: the waiter's budget is wall time, so a slice that returns immediately turns a map
  // that is merely not ready yet into a burst of fast calls against that budget.
  //
  // Microseconds, not milliseconds: the readback path slices below a millisecond, and rounding
  // a 100 us slice up to 1 ms would coarsen the cadence that path is tuned to.
  const auto slice = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::duration<double>(sliceSeconds));

  (void)BoundedGpuWait(
      [&] {
        root_->device().poll(false, nullptr);
        return !mappingStillMatches(mappingSlotIndex, &completion, future) ||
               completion.done.load(std::memory_order_acquire) || isLost();
      },
      std::max(slice, std::chrono::microseconds(1)));

  return gpu::MapSliceReport{.state = mappingStillMatches(mappingSlotIndex, &completion, future)
                                          ? sliceStateOf(completion)
                                          : gpu::MapSliceState::Failed,
                             .waitKind = gpu::MapWaitKind::Polled};
}

gpu::Result<std::span<const uint8_t>> GeodeWgpuAdapterDevice::onMappedBytes(
    uint32_t mappingSlotIndex) const {
  if (mappingSlotIndex >= slotMappings_.size() ||
      slotMappings_[mappingSlotIndex].completion == nullptr) {
    return GpuError{GpuErrorType::InvalidState, "the mapping is no longer live"};
  }
  const MappingSlot& slot = slotMappings_[mappingSlotIndex];
  if (!slot.completion->done.load(std::memory_order_acquire) ||
      !slot.completion->ok.load(std::memory_order_relaxed)) {
    return GpuError{GpuErrorType::InvalidState, "the mapping has not completed"};
  }
  const void* mapped = slot.buffer.getConstMappedRange(slot.offsetBytes, slot.byteCount);
  if (mapped == nullptr) {
    return GpuError{GpuErrorType::InvalidState, "the backend returned no mapped range"};
  }
  return std::span<const uint8_t>(static_cast<const uint8_t*>(mapped),
                                  static_cast<size_t>(slot.byteCount));
}

void GeodeWgpuAdapterDevice::onUnmapBuffer(uint32_t mappingSlotIndex) {
  if (mappingSlotIndex >= slotMappings_.size() ||
      slotMappings_[mappingSlotIndex].completion == nullptr) {
    return;
  }
  MappingSlot& slot = slotMappings_[mappingSlotIndex];
  // From here the completion is the only thing that can give the buffer back, whether the map
  // has already finished or is still in flight.
  slot.completion->abandoned.store(true, std::memory_order_release);
  slot.completion->unmapIfAbandoned();
  // The pending callback holds the other reference and releases it when it runs, so a map that
  // is still in flight when the mapping is dropped cannot leave the state behind either way.
  slot.completion->release();
  slot.completion = nullptr;
  slot.buffer = wgpu::Buffer();
}

gpu::Status GeodeWgpuAdapterDevice::onCreateBuffer(uint32_t slotIndex,
                                                   const gpu::BufferDescriptor& descriptor) {
  wgpu::BufferDescriptor bufferDescriptor = {};
  bufferDescriptor.label = wgpuLabel(std::string_view(descriptor.label));
  bufferDescriptor.size = descriptor.byteSize;
  bufferDescriptor.usage = ToWgpuBufferUsage(descriptor.usage);

  wgpu::Buffer buffer = root_->device().createBuffer(bufferDescriptor);
  if (!buffer) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("wgpu buffer allocation of {} bytes failed for '{}'",
                                descriptor.byteSize, std::string_view(descriptor.label))};
  }
  SetSlot(slotBuffers_, slotIndex, ScopedWgpuHandle<wgpu::Buffer>(buffer));
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::onCreateTexture(uint32_t slotIndex,
                                                    const gpu::TextureDescriptor& descriptor) {
  wgpu::TextureDescriptor textureDescriptor = {};
  textureDescriptor.label = wgpuLabel(std::string_view(descriptor.label));
  textureDescriptor.size = {descriptor.size.width, descriptor.size.height, 1u};
  textureDescriptor.format = WgpuTextureFormatFrom(descriptor.format);
  textureDescriptor.usage = ToWgpuTextureUsage(descriptor.usage);
  textureDescriptor.mipLevelCount = 1;
  textureDescriptor.sampleCount = 1;
  textureDescriptor.dimension = wgpu::TextureDimension::_2D;

  wgpu::Texture texture = root_->device().createTexture(textureDescriptor);
  if (!texture) {
    return GpuError{
        GpuErrorType::InvalidState,
        std::format("wgpu texture allocation ({}x{}) failed for '{}'", descriptor.size.width,
                    descriptor.size.height, std::string_view(descriptor.label))};
  }
  SetSlot(slotTextures_, slotIndex, TextureSlot{ScopedWgpuHandle<wgpu::Texture>(texture), texture});
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::onCreateTextureView(
    uint32_t slotIndex, uint32_t textureSlotIndex, const gpu::TextureViewDescriptor& descriptor) {
  (void)descriptor;  // Views cover the whole texture; wgpu's default view matches.
  wgpu::Texture texture = textureSlotIndex < slotTextures_.size()
                              ? slotTextures_[textureSlotIndex].texture
                              : wgpu::Texture();
  if (!texture) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("texture slot {} has no wgpu texture to view", textureSlotIndex)};
  }

  wgpu::TextureView view = texture.createView();
  if (!view) {
    return GpuError{
        GpuErrorType::InvalidState,
        std::format("wgpu texture view creation failed for texture slot {}", textureSlotIndex)};
  }

  SetSlot(slotTextureViews_, slotIndex, ScopedWgpuHandle<wgpu::TextureView>(view));
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::onCreateSampler(uint32_t slotIndex,
                                                    const gpu::SamplerDescriptor& descriptor) {
  // `{wgpu::Default}` fills lodMaxClamp and, critically, `maxAnisotropy = 1`, which wgpu-native
  // validates as non-zero (see GeodeImagePipeline's sampler creation).
  wgpu::SamplerDescriptor samplerDescriptor{wgpu::Default};
  samplerDescriptor.label = wgpuLabel(std::string_view(descriptor.label));
  samplerDescriptor.magFilter = ToWgpuFilterMode(descriptor.magFilter);
  samplerDescriptor.minFilter = ToWgpuFilterMode(descriptor.minFilter);
  samplerDescriptor.addressModeU = ToWgpuAddressMode(descriptor.addressModeU);
  samplerDescriptor.addressModeV = ToWgpuAddressMode(descriptor.addressModeV);
  samplerDescriptor.maxAnisotropy = 1;

  wgpu::Sampler sampler = root_->device().createSampler(samplerDescriptor);
  if (!sampler) {
    return GpuError{GpuErrorType::InvalidState, std::format("wgpu sampler creation failed for '{}'",
                                                            std::string_view(descriptor.label))};
  }

  SetSlot(slotSamplers_, slotIndex, ScopedWgpuHandle<wgpu::Sampler>(sampler));
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::onCreateBindGroupLayout(
    uint32_t slotIndex, const gpu::BindGroupLayoutDescriptor& descriptor) {
  std::vector<wgpu::BindGroupLayoutEntry> entries(descriptor.entries.size());
  for (size_t i = 0; i < descriptor.entries.size(); ++i) {
    const gpu::BindGroupLayoutEntry& entry = descriptor.entries[i];
    entries[i].binding = entry.binding;
    entries[i].visibility = ToWgpuShaderStage(entry.visibility);
    ApplyBindingType(entries[i], entry);
  }

  wgpu::BindGroupLayoutDescriptor layoutDescriptor = {};
  layoutDescriptor.label = wgpuLabel(std::string_view(descriptor.label));
  layoutDescriptor.entryCount = entries.size();
  layoutDescriptor.entries = entries.data();

  wgpu::BindGroupLayout layout = root_->device().createBindGroupLayout(layoutDescriptor);
  if (!layout) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("wgpu bind group layout creation failed for '{}'",
                                std::string_view(descriptor.label))};
  }

  SetSlot(slotBindGroupLayouts_, slotIndex, ScopedWgpuHandle<wgpu::BindGroupLayout>(layout));
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::onCreateBindGroup(uint32_t slotIndex,
                                                      const gpu::BindGroupDescriptor& descriptor) {
  // Creation-time layout lookup only (the base class just validated the reference); the created
  // wgpu bind group retains its layout internally, so encoding never resolves layouts by slot.
  wgpu::BindGroupLayout layout = GetHandle(slotBindGroupLayouts_, descriptor.layout.slotIndex());
  if (!layout) {
    return GpuError{
        GpuErrorType::InvalidState,
        std::format("bind group layout slot {} has no wgpu layout", descriptor.layout.slotIndex())};
  }

  std::vector<wgpu::BindGroupEntry> entries(descriptor.entries.size());
  for (size_t i = 0; i < descriptor.entries.size(); ++i) {
    const gpu::BindGroupEntry& entry = descriptor.entries[i];
    entries[i].binding = entry.binding;
    if (const gpu::BufferBinding* bufferBinding =
            std::get_if<gpu::BufferBinding>(&entry.resource)) {
      wgpu::Buffer buffer = GetHandle(slotBuffers_, bufferBinding->buffer.slotIndex());
      if (!buffer) {
        return GpuError{GpuErrorType::InvalidState,
                        std::format("bind group entry binding {} does not resolve to a wgpu "
                                    "buffer",
                                    entry.binding)};
      }
      entries[i].buffer = buffer;
      entries[i].offset = bufferBinding->offsetBytes;
      entries[i].size = bufferBinding->sizeBytes;
    } else if (const gpu::TextureViewBinding* viewBinding =
                   std::get_if<gpu::TextureViewBinding>(&entry.resource)) {
      wgpu::TextureView view = GetHandle(slotTextureViews_, viewBinding->view.slotIndex());
      if (!view) {
        return GpuError{GpuErrorType::InvalidState,
                        std::format("bind group entry binding {} does not resolve to a wgpu "
                                    "texture view",
                                    entry.binding)};
      }
      entries[i].textureView = view;
    } else if (const gpu::SamplerBinding* samplerBinding =
                   std::get_if<gpu::SamplerBinding>(&entry.resource)) {
      wgpu::Sampler sampler = GetHandle(slotSamplers_, samplerBinding->sampler.slotIndex());
      if (!sampler) {
        return GpuError{GpuErrorType::InvalidState,
                        std::format("bind group entry binding {} does not resolve to a wgpu "
                                    "sampler",
                                    entry.binding)};
      }
      entries[i].sampler = sampler;
    }
  }

  wgpu::BindGroupDescriptor groupDescriptor = {};
  groupDescriptor.label = wgpuLabel(std::string_view(descriptor.label));
  groupDescriptor.layout = layout;
  groupDescriptor.entryCount = entries.size();
  groupDescriptor.entries = entries.data();

  wgpu::BindGroup group = root_->device().createBindGroup(groupDescriptor);
  if (!group) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("wgpu bind group creation failed for '{}'",
                                std::string_view(descriptor.label))};
  }

  SetSlot(slotBindGroups_, slotIndex, ScopedWgpuHandle<wgpu::BindGroup>(group));
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::onCreatePipelineLayout(
    uint32_t slotIndex, const gpu::PipelineLayoutDescriptor& descriptor) {
  std::vector<WGPUBindGroupLayout> layouts(descriptor.bindGroupLayouts.size());
  for (size_t i = 0; i < descriptor.bindGroupLayouts.size(); ++i) {
    wgpu::BindGroupLayout layout =
        GetHandle(slotBindGroupLayouts_, descriptor.bindGroupLayouts[i].slotIndex());
    if (!layout) {
      return GpuError{GpuErrorType::InvalidState,
                      std::format("pipeline layout references bind group layout slot {} with no "
                                  "wgpu layout",
                                  descriptor.bindGroupLayouts[i].slotIndex())};
    }
    layouts[i] = layout;
  }

  wgpu::PipelineLayoutDescriptor layoutDescriptor = {};
  layoutDescriptor.label = wgpuLabel(std::string_view(descriptor.label));
  layoutDescriptor.bindGroupLayoutCount = layouts.size();
  layoutDescriptor.bindGroupLayouts = layouts.data();

  wgpu::PipelineLayout layout = root_->device().createPipelineLayout(layoutDescriptor);
  if (!layout) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("wgpu pipeline layout creation failed for '{}'",
                                std::string_view(descriptor.label))};
  }

  SetSlot(slotPipelineLayouts_, slotIndex, ScopedWgpuHandle<wgpu::PipelineLayout>(layout));
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::onCreateShaderModule(
    uint32_t slotIndex, const gpu::ShaderModuleDescriptor& descriptor) {
  if (descriptor.sourceKind != gpu::ShaderSourceKind::Wgsl) {
    return GpuError{GpuErrorType::Unsupported, "the wgpu adapter compiles WGSL only"};
  }

  // Same WGSL chaining as GeodeShaders.cc's createShaderFromWgsl: the source text rides a
  // ShaderSourceWGSL chained struct whose sType `setDefault()` fills in.
  const std::string_view source(descriptor.sourceText);
  wgpu::ShaderSourceWGSL wgslSource{wgpu::Default};
  wgslSource.code.data = source.data();
  wgslSource.code.length = source.size();

  wgpu::ShaderModuleDescriptor moduleDescriptor{wgpu::Default};
  moduleDescriptor.label = wgpuLabel(std::string_view(descriptor.label));
  moduleDescriptor.nextInChain = &wgslSource.chain;

  wgpu::ShaderModule module = root_->device().createShaderModule(moduleDescriptor);
  if (!module) {
    return GpuError{GpuErrorType::InvalidDescriptor,
                    std::format("wgpu shader module creation failed for '{}'",
                                std::string_view(descriptor.label))};
  }

  SetSlot(slotShaderModules_, slotIndex, ScopedWgpuHandle<wgpu::ShaderModule>(module));
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::onCreateRenderPipeline(
    uint32_t slotIndex, const gpu::RenderPipelineDescriptor& descriptor) {
  wgpu::PipelineLayout layout = GetHandle(slotPipelineLayouts_, descriptor.layout.slotIndex());
  wgpu::ShaderModule vertexModule =
      GetHandle(slotShaderModules_, descriptor.vertex.module.slotIndex());
  wgpu::ShaderModule fragmentModule =
      GetHandle(slotShaderModules_, descriptor.fragment.module.slotIndex());
  if (!layout || !vertexModule || !fragmentModule) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("render pipeline '{}' references a layout or shader module with "
                                "no wgpu object",
                                std::string_view(descriptor.label))};
  }

  // Per-buffer attribute arrays must stay alive until createRenderPipeline returns.
  std::vector<std::vector<wgpu::VertexAttribute>> attributeStorage(
      descriptor.vertex.buffers.size());
  std::vector<wgpu::VertexBufferLayout> vertexBuffers(descriptor.vertex.buffers.size());
  for (size_t i = 0; i < descriptor.vertex.buffers.size(); ++i) {
    const gpu::VertexBufferLayout& bufferLayout = descriptor.vertex.buffers[i];
    attributeStorage[i].resize(bufferLayout.attributes.size());
    for (size_t j = 0; j < bufferLayout.attributes.size(); ++j) {
      const gpu::VertexAttribute& attribute = bufferLayout.attributes[j];
      attributeStorage[i][j].format = ToWgpuVertexFormat(attribute.format);
      attributeStorage[i][j].offset = attribute.offsetBytes;
      attributeStorage[i][j].shaderLocation = attribute.shaderLocation;
    }

    vertexBuffers[i].arrayStride = bufferLayout.strideBytes;
    vertexBuffers[i].stepMode = ToWgpuVertexStepMode(bufferLayout.stepMode);
    vertexBuffers[i].attributeCount = attributeStorage[i].size();
    vertexBuffers[i].attributes = attributeStorage[i].data();
  }

  std::vector<wgpu::BlendState> blendStorage(descriptor.fragment.targets.size());
  std::vector<wgpu::ColorTargetState> targets(descriptor.fragment.targets.size());
  for (size_t i = 0; i < descriptor.fragment.targets.size(); ++i) {
    const gpu::ColorTargetState& target = descriptor.fragment.targets[i];
    targets[i].format = WgpuTextureFormatFrom(target.format);
    targets[i].writeMask = ToWgpuColorWriteMask(target.writeMask);
    if (target.blend.has_value()) {
      blendStorage[i].color.srcFactor = ToWgpuBlendFactor(target.blend->color.srcFactor);
      blendStorage[i].color.dstFactor = ToWgpuBlendFactor(target.blend->color.dstFactor);
      blendStorage[i].color.operation = ToWgpuBlendOperation(target.blend->color.operation);
      blendStorage[i].alpha.srcFactor = ToWgpuBlendFactor(target.blend->alpha.srcFactor);
      blendStorage[i].alpha.dstFactor = ToWgpuBlendFactor(target.blend->alpha.dstFactor);
      blendStorage[i].alpha.operation = ToWgpuBlendOperation(target.blend->alpha.operation);
      targets[i].blend = &blendStorage[i];
    }
  }

  const std::string_view vertexEntryPoint(descriptor.vertex.entryPoint);
  const std::string_view fragmentEntryPoint(descriptor.fragment.entryPoint);

  wgpu::FragmentState fragmentState = {};
  fragmentState.module = fragmentModule;
  fragmentState.entryPoint = wgpuLabel(fragmentEntryPoint);
  fragmentState.targetCount = targets.size();
  fragmentState.targets = targets.data();

  wgpu::RenderPipelineDescriptor pipelineDescriptor = {};
  pipelineDescriptor.label = wgpuLabel(std::string_view(descriptor.label));
  pipelineDescriptor.layout = layout;
  pipelineDescriptor.vertex.module = vertexModule;
  pipelineDescriptor.vertex.entryPoint = wgpuLabel(vertexEntryPoint);
  pipelineDescriptor.vertex.bufferCount = vertexBuffers.size();
  pipelineDescriptor.vertex.buffers = vertexBuffers.empty() ? nullptr : vertexBuffers.data();
  pipelineDescriptor.primitive.topology = ToWgpuPrimitiveTopology(descriptor.topology);
  pipelineDescriptor.primitive.cullMode = ToWgpuCullMode(descriptor.cullMode);
  pipelineDescriptor.fragment = &fragmentState;
  pipelineDescriptor.multisample.count = 1;
  pipelineDescriptor.multisample.mask = 0xFFFFFFFF;

  wgpu::RenderPipeline pipeline = root_->device().createRenderPipeline(pipelineDescriptor);
  if (!pipeline) {
    return GpuError{GpuErrorType::InvalidDescriptor,
                    std::format("wgpu render pipeline creation failed for '{}'",
                                std::string_view(descriptor.label))};
  }

  SetSlot(slotRenderPipelines_, slotIndex, ScopedWgpuHandle<wgpu::RenderPipeline>(pipeline));
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::onCreateComputePipeline(
    uint32_t slotIndex, const gpu::ComputePipelineDescriptor& descriptor) {
  wgpu::PipelineLayout layout = GetHandle(slotPipelineLayouts_, descriptor.layout.slotIndex());
  wgpu::ShaderModule module = GetHandle(slotShaderModules_, descriptor.compute.module.slotIndex());
  if (!layout || !module) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("compute pipeline '{}' references a layout or shader module with "
                                "no wgpu object",
                                std::string_view(descriptor.label))};
  }

  const std::string_view entryPoint(descriptor.compute.entryPoint);
  wgpu::ComputePipelineDescriptor pipelineDescriptor = {};
  pipelineDescriptor.label = wgpuLabel(std::string_view(descriptor.label));
  pipelineDescriptor.layout = layout;
  pipelineDescriptor.compute.module = module;
  pipelineDescriptor.compute.entryPoint = wgpuLabel(entryPoint);

  wgpu::ComputePipeline pipeline = root_->device().createComputePipeline(pipelineDescriptor);
  if (!pipeline) {
    return GpuError{GpuErrorType::InvalidDescriptor,
                    std::format("wgpu compute pipeline creation failed for '{}'",
                                std::string_view(descriptor.label))};
  }

  SetSlot(slotComputePipelines_, slotIndex, ScopedWgpuHandle<wgpu::ComputePipeline>(pipeline));
  return OkStatus();
}

/// Clears the slot of one non-pipeline resource kind, returning false when \p resourceName names
/// no kind this table set tracks.
/// @param resourceName Resource type name from the base class.
/// @param slotIndex Slot to clear.
bool GeodeWgpuAdapterDevice::clearResourceSlot(std::string_view resourceName, uint32_t slotIndex) {
  if (resourceName == "buffer") {
    SetSlot(slotBuffers_, slotIndex, ScopedWgpuHandle<wgpu::Buffer>());
  } else if (resourceName == "texture") {
    SetSlot(slotTextures_, slotIndex, TextureSlot{});
  } else if (resourceName == "textureView") {
    SetSlot(slotTextureViews_, slotIndex, ScopedWgpuHandle<wgpu::TextureView>());
  } else if (resourceName == "sampler") {
    SetSlot(slotSamplers_, slotIndex, ScopedWgpuHandle<wgpu::Sampler>());
  } else if (resourceName == "bindGroupLayout") {
    SetSlot(slotBindGroupLayouts_, slotIndex, ScopedWgpuHandle<wgpu::BindGroupLayout>());
  } else if (resourceName == "bindGroup") {
    SetSlot(slotBindGroups_, slotIndex, ScopedWgpuHandle<wgpu::BindGroup>());
  } else {
    return false;
  }
  return true;
}

/// Clears the slot of one pipeline-family resource kind, returning false when \p resourceName
/// names no kind this table set tracks.
/// @param resourceName Resource type name from the base class.
/// @param slotIndex Slot to clear.
bool GeodeWgpuAdapterDevice::clearPipelineSlot(std::string_view resourceName, uint32_t slotIndex) {
  if (resourceName == "pipelineLayout") {
    SetSlot(slotPipelineLayouts_, slotIndex, ScopedWgpuHandle<wgpu::PipelineLayout>());
  } else if (resourceName == "shaderModule") {
    SetSlot(slotShaderModules_, slotIndex, ScopedWgpuHandle<wgpu::ShaderModule>());
  } else if (resourceName == "renderPipeline") {
    SetSlot(slotRenderPipelines_, slotIndex, ScopedWgpuHandle<wgpu::RenderPipeline>());
  } else if (resourceName == "computePipeline") {
    SetSlot(slotComputePipelines_, slotIndex, ScopedWgpuHandle<wgpu::ComputePipeline>());
  } else {
    return false;
  }
  return true;
}

void GeodeWgpuAdapterDevice::onDestroyResource(std::string_view resourceName, uint32_t slotIndex) {
  // Clearing a slot releases the owned wgpu reference (imported external textures carry no
  // owned reference, so their backing object is untouched).
  if (clearResourceSlot(resourceName, slotIndex) || clearPipelineSlot(resourceName, slotIndex)) {
    return;
  }
  // A resource kind this adapter does not track would leak its wgpu object silently. Loud in
  // debug so a new kind added to the runtime is wired up here; release-safe no-op (the base
  // class owns the bookkeeping either way).
  assert(false && "GeodeWgpuAdapterDevice::onDestroyResource: unknown resource kind");
}

gpu::Status GeodeWgpuAdapterDevice::onWriteBuffer(uint32_t slotIndex, uint64_t offsetBytes,
                                                  std::span<const uint8_t> data) {
  wgpu::Buffer buffer = GetHandle(slotBuffers_, slotIndex);
  if (!buffer) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("buffer slot {} has no wgpu buffer", slotIndex)};
  }
  // WebGPU's writeBuffer requires 4-byte-aligned offset and size ("GPUQueue.writeBuffer"
  // validation); fail closed here instead of surfacing an asynchronous device error.
  if (offsetBytes % 4 != 0 || data.size() % 4 != 0) {
    return GpuError{GpuErrorType::Unsupported,
                    std::format("writeBuffer: offsetBytes {} / byteCount {} must be 4-byte "
                                "aligned for the wgpu adapter",
                                offsetBytes, data.size())};
  }

  root_->queue().writeBuffer(buffer, offsetBytes, data.data(), data.size());
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::onWriteTexture(uint32_t slotIndex,
                                                   std::span<const uint8_t> data,
                                                   const gpu::TexelCopyBufferLayout& dataLayout,
                                                   const gpu::Extent2d& writeSize,
                                                   const gpu::Origin2d& destinationOrigin) {
  wgpu::Texture texture =
      slotIndex < slotTextures_.size() ? slotTextures_[slotIndex].texture : wgpu::Texture();
  if (!texture) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("texture slot {} has no wgpu texture", slotIndex)};
  }

  SmallVector<uint8_t, gpu::kTexelRowPitchAlignment> ownedData;
  std::span<const uint8_t> uploadData;
  gpu::TexelCopyBufferLayout uploadLayout;
  if (gpu::Status status = PrepareWgpuTextureUpload(texture.getFormat(), data, dataLayout,
                                                    writeSize, ownedData, uploadData, uploadLayout);
      status.hasError()) {
    return status;
  }

  wgpu::TexelCopyTextureInfo destination = {};
  destination.texture = texture;
  destination.origin = {destinationOrigin.x, destinationOrigin.y, 0u};
  wgpu::TexelCopyBufferLayout layout = {};
  layout.offset = uploadLayout.offsetBytes;
  layout.bytesPerRow = uploadLayout.bytesPerRow;
  layout.rowsPerImage = uploadLayout.rowsPerImage;
  const wgpu::Extent3D extent = {writeSize.width, writeSize.height, 1u};
  root_->queue().writeTexture(destination, uploadData.data(), uploadData.size(), layout, extent);
  lastTextureUploadBytes_ = uploadData.size();
  return OkStatus();
}

uint64_t GeodeWgpuAdapterDevice::onTextureWriteByteCount(std::span<const uint8_t> /*data*/) const {
  return lastTextureUploadBytes_;
}

gpu::Status GeodeWgpuAdapterDevice::encodeBeginRenderPass(
    EncodingState& state, const gpu::BeginRenderPassCommand& beginPass) {
  const auto& attachments = beginPass.descriptor.colorAttachments;
  std::vector<wgpu::RenderPassColorAttachment> colorAttachments(attachments.size());
  for (size_t i = 0; i < attachments.size(); ++i) {
    const gpu::RenderPassColorAttachment& attachment = attachments[i];
    wgpu::TextureView view = GetHandle(slotTextureViews_, attachment.view.slotIndex());
    if (!view) {
      return GpuError{
          GpuErrorType::InvalidState,
          std::format("render pass attachment {} does not resolve to a wgpu texture view", i)};
    }
    colorAttachments[i].view = view;
    colorAttachments[i].loadOp = ToWgpuLoadOp(attachment.loadOp);
    colorAttachments[i].storeOp = ToWgpuStoreOp(attachment.storeOp);
    colorAttachments[i].clearValue = {attachment.clearColor[0], attachment.clearColor[1],
                                      attachment.clearColor[2], attachment.clearColor[3]};
    // Dawn (browser WebGPU) rejects depthSlice=0 on non-3D views; wgpu-native is lenient. Set
    // the UNDEFINED sentinel for cross-backend compatibility (see GeoEncoder).
    colorAttachments[i].depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
  }

  wgpu::RenderPassDescriptor passDescriptor = {};
  passDescriptor.label = wgpuLabel(std::string_view(beginPass.descriptor.label));
  passDescriptor.colorAttachmentCount = colorAttachments.size();
  passDescriptor.colorAttachments = colorAttachments.data();
  state.pass.reset(state.encoder.get().beginRenderPass(passDescriptor));
  if (!state.pass) {
    return GpuError{GpuErrorType::InvalidState, "wgpu render pass creation failed"};
  }
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::encodeSetPipeline(EncodingState& state,
                                                      const gpu::SetPipelineCommand& setPipeline) {
  wgpu::RenderPipeline pipeline = GetHandle(slotRenderPipelines_, setPipeline.pipelineId.slotIndex);
  if (!state.pass || !pipeline) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("setPipeline: pipeline slot {} is not encodable",
                                setPipeline.pipelineId.slotIndex)};
  }
  state.pass.get().setPipeline(pipeline);
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::encodeSetBindGroup(
    EncodingState& state, const gpu::SetBindGroupCommand& setBindGroup) {
  wgpu::BindGroup group = GetHandle(slotBindGroups_, setBindGroup.bindGroupId.slotIndex);
  if ((!state.pass && !state.computePass) || !group) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("setBindGroup: bind group slot {} is not encodable",
                                setBindGroup.bindGroupId.slotIndex)};
  }
  if (state.pass) {
    state.pass.get().setBindGroup(setBindGroup.index, group, 0, nullptr);
  } else {
    state.computePass.get().setBindGroup(setBindGroup.index, group, 0, nullptr);
  }
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::encodeSetVertexBuffer(
    EncodingState& state, const gpu::SetVertexBufferCommand& setVertexBuffer) {
  wgpu::Buffer buffer = GetHandle(slotBuffers_, setVertexBuffer.bufferId.slotIndex);
  if (!state.pass || !buffer) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("setVertexBuffer: buffer slot {} is not encodable",
                                setVertexBuffer.bufferId.slotIndex)};
  }
  state.pass.get().setVertexBuffer(setVertexBuffer.slot, buffer, setVertexBuffer.offsetBytes,
                                   WGPU_WHOLE_SIZE);
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::encodeSetIndexBuffer(
    EncodingState& state, const gpu::SetIndexBufferCommand& setIndexBuffer) {
  wgpu::Buffer buffer = GetHandle(slotBuffers_, setIndexBuffer.bufferId.slotIndex);
  if (!state.pass || !buffer) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("setIndexBuffer: buffer slot {} is not encodable",
                                setIndexBuffer.bufferId.slotIndex)};
  }
  state.pass.get().setIndexBuffer(buffer, ToWgpuIndexFormat(setIndexBuffer.format),
                                  setIndexBuffer.offsetBytes, WGPU_WHOLE_SIZE);
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::encodeSetScissorRect(
    EncodingState& state, const gpu::SetScissorRectCommand& setScissor) {
  if (!state.pass) {
    return GpuError{GpuErrorType::InvalidState, "setScissorRect outside a render pass"};
  }
  state.pass.get().setScissorRect(setScissor.x, setScissor.y, setScissor.width, setScissor.height);
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::encodeSetViewport(EncodingState& state,
                                                      const gpu::SetViewportCommand& setViewport) {
  if (!state.pass) {
    return GpuError{GpuErrorType::InvalidState, "setViewport outside a render pass"};
  }
  state.pass.get().setViewport(setViewport.x, setViewport.y, setViewport.width, setViewport.height,
                               setViewport.minDepth, setViewport.maxDepth);
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::encodeDraw(EncodingState& state, const gpu::DrawCommand& draw) {
  if (!state.pass) {
    return GpuError{GpuErrorType::InvalidState, "draw outside a render pass"};
  }
  state.pass.get().draw(draw.vertexCount, draw.instanceCount, draw.firstVertex, draw.firstInstance);
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::encodeDrawIndexed(EncodingState& state,
                                                      const gpu::DrawIndexedCommand& draw) {
  if (!state.pass) {
    return GpuError{GpuErrorType::InvalidState, "drawIndexed outside a render pass"};
  }
  if (gpu::IsEmptyIndexedDraw(draw)) {
    return OkStatus();
  }
  state.pass.get().drawIndexed(draw.indexCount, draw.instanceCount, draw.firstIndex,
                               draw.baseVertex, draw.firstInstance);
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::encodeEndRenderPass(EncodingState& state) {
  if (!state.pass) {
    return GpuError{GpuErrorType::InvalidState, "endRenderPass without an active render pass"};
  }
  state.pass.get().end();
  state.pass.reset();
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::encodeBeginComputePass(
    EncodingState& state, const gpu::BeginComputePassCommand& beginPass) {
  wgpu::ComputePassDescriptor passDescriptor = {};
  passDescriptor.label = wgpuLabel(std::string_view(beginPass.descriptor.label));
  state.computePass.reset(state.encoder.get().beginComputePass(passDescriptor));
  if (!state.computePass) {
    return GpuError{GpuErrorType::InvalidState, "wgpu compute pass creation failed"};
  }
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::encodeSetComputePipeline(
    EncodingState& state, const gpu::SetComputePipelineCommand& setPipeline) {
  wgpu::ComputePipeline pipeline =
      GetHandle(slotComputePipelines_, setPipeline.pipelineId.slotIndex);
  if (!state.computePass || !pipeline) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("setPipeline: compute pipeline slot {} is not encodable",
                                setPipeline.pipelineId.slotIndex)};
  }
  state.computePass.get().setPipeline(pipeline);
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::encodeDispatchWorkgroups(
    EncodingState& state, const gpu::DispatchWorkgroupsCommand& dispatch) {
  if (!state.computePass) {
    return GpuError{GpuErrorType::InvalidState, "dispatchWorkgroups outside a compute pass"};
  }
  state.computePass.get().dispatchWorkgroups(dispatch.workgroupCountX, dispatch.workgroupCountY,
                                             dispatch.workgroupCountZ);
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::encodeEndComputePass(EncodingState& state) {
  if (!state.computePass) {
    return GpuError{GpuErrorType::InvalidState, "endComputePass without an active compute pass"};
  }
  state.computePass.get().end();
  state.computePass.reset();
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::encodeCopyTextureToBuffer(
    EncodingState& state, const gpu::CopyTextureToBufferCommand& copy) {
  if (state.pass || state.computePass) {
    return GpuError{GpuErrorType::InvalidState, "copyTextureToBuffer inside a pass"};
  }
  wgpu::Texture texture = copy.textureId.slotIndex < slotTextures_.size()
                              ? slotTextures_[copy.textureId.slotIndex].texture
                              : wgpu::Texture();
  wgpu::Buffer buffer = GetHandle(slotBuffers_, copy.bufferId.slotIndex);
  if (!texture || !buffer) {
    return GpuError{GpuErrorType::InvalidState,
                    "copyTextureToBuffer: source texture or destination buffer is missing"};
  }
  wgpu::TexelCopyTextureInfo source = {};
  source.texture = texture;
  wgpu::TexelCopyBufferInfo destination = {};
  destination.buffer = buffer;
  destination.layout.offset = copy.layout.offsetBytes;
  destination.layout.bytesPerRow = copy.layout.bytesPerRow;
  destination.layout.rowsPerImage = copy.layout.rowsPerImage;
  const wgpu::Extent3D extent = {copy.copySize.width, copy.copySize.height, 1u};
  state.encoder.get().copyTextureToBuffer(source, destination, extent);
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::encodeCopyTextureToTexture(
    EncodingState& state, const gpu::CopyTextureToTextureCommand& textureCopy) {
  if (state.pass || state.computePass) {
    return GpuError{GpuErrorType::InvalidState, "copyTextureToTexture inside a pass"};
  }
  wgpu::Texture sourceTexture = textureCopy.textureSrcId.slotIndex < slotTextures_.size()
                                    ? slotTextures_[textureCopy.textureSrcId.slotIndex].texture
                                    : wgpu::Texture();
  wgpu::Texture destinationTexture = textureCopy.textureDstId.slotIndex < slotTextures_.size()
                                         ? slotTextures_[textureCopy.textureDstId.slotIndex].texture
                                         : wgpu::Texture();
  if (!sourceTexture || !destinationTexture) {
    return GpuError{GpuErrorType::InvalidState,
                    "copyTextureToTexture: source or destination texture is missing"};
  }
  wgpu::TexelCopyTextureInfo source = {};
  source.texture = sourceTexture;
  source.origin = {textureCopy.sourceOrigin.x, textureCopy.sourceOrigin.y, 0u};
  wgpu::TexelCopyTextureInfo destination = {};
  destination.texture = destinationTexture;
  destination.origin = {textureCopy.destinationOrigin.x, textureCopy.destinationOrigin.y, 0u};
  const wgpu::Extent3D extent = {textureCopy.copySize.width, textureCopy.copySize.height, 1u};
  state.encoder.get().copyTextureToTexture(source, destination, extent);
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::encodeCommand(EncodingState& state,
                                                  const gpu::Command& command) {
  // Exhaustive dispatch: every `gpu::Command` alternative has a handler, so adding a new
  // command to the variant without wiring it here is a compile error instead of a validated,
  // submitted, "completed", never-executed no-op.
  return std::visit(
      Overloaded{
          [&](const gpu::BeginRenderPassCommand& beginPass) -> gpu::Status {
            return encodeBeginRenderPass(state, beginPass);
          },
          [&](const gpu::SetPipelineCommand& setPipeline) -> gpu::Status {
            return encodeSetPipeline(state, setPipeline);
          },
          [&](const gpu::SetBindGroupCommand& setBindGroup) -> gpu::Status {
            return encodeSetBindGroup(state, setBindGroup);
          },
          [&](const gpu::SetVertexBufferCommand& setVertexBuffer) -> gpu::Status {
            return encodeSetVertexBuffer(state, setVertexBuffer);
          },
          [&](const gpu::SetIndexBufferCommand& setIndexBuffer) -> gpu::Status {
            return encodeSetIndexBuffer(state, setIndexBuffer);
          },
          [&](const gpu::SetScissorRectCommand& setScissor) -> gpu::Status {
            return encodeSetScissorRect(state, setScissor);
          },
          [&](const gpu::SetViewportCommand& setViewport) -> gpu::Status {
            return encodeSetViewport(state, setViewport);
          },
          [&](const gpu::DrawCommand& draw) -> gpu::Status { return encodeDraw(state, draw); },
          [&](const gpu::DrawIndexedCommand& draw) -> gpu::Status {
            return encodeDrawIndexed(state, draw);
          },
          [&](const gpu::EndRenderPassCommand&) -> gpu::Status {
            return encodeEndRenderPass(state);
          },
          [&](const gpu::CopyTextureToBufferCommand& copy) -> gpu::Status {
            return encodeCopyTextureToBuffer(state, copy);
          },
          [&](const gpu::CopyTextureToTextureCommand& textureCopy) -> gpu::Status {
            return encodeCopyTextureToTexture(state, textureCopy);
          },
          [&](const gpu::BeginComputePassCommand& beginPass) -> gpu::Status {
            return encodeBeginComputePass(state, beginPass);
          },
          [&](const gpu::SetComputePipelineCommand& setPipeline) -> gpu::Status {
            return encodeSetComputePipeline(state, setPipeline);
          },
          [&](const gpu::DispatchWorkgroupsCommand& dispatch) -> gpu::Status {
            return encodeDispatchWorkgroups(state, dispatch);
          },
          [&](const gpu::EndComputePassCommand&) -> gpu::Status {
            return encodeEndComputePass(state);
          },
      },
      command);
}

void GeodeWgpuAdapterDevice::holdSubmittedWorkForTesting(uint64_t completedSerialCeiling) {
  completedSerialCeiling_.store(completedSerialCeiling, std::memory_order_relaxed);
  completionState_->notifyProgress();
}

void GeodeWgpuAdapterDevice::CompletionState::notifyProgress() {
  // Taking the lock orders the publication before the wake: a wait that has just checked and is
  // about to sleep holds it, so it either sees what was published or is asleep for the signal.
  { std::scoped_lock lock(mutex); }
  progressed.notify_all();
}

void GeodeWgpuAdapterDevice::CompletionState::record(uint64_t serial) {
  std::scoped_lock lock(mutex);
  pending.push_back(Pending{serial, serial});
}

void GeodeWgpuAdapterDevice::CompletionState::complete(uint64_t ticket) {
  std::scoped_lock lock(mutex);
  const auto range = std::find_if(pending.begin(), pending.end(), [ticket](const Pending& entry) {
    return entry.firstSerial == ticket;
  });
  if (range == pending.end()) {
    return;
  }
  completedHighWater = std::max(completedHighWater, range->lastSerial);
  *range = pending.back();
  pending.pop_back();
  uint64_t prefix = completedHighWater;
  for (const Pending& unfinished : pending) {
    prefix = std::min(prefix, unfinished.firstSerial - 1);
  }
  completedSerial.store(prefix, std::memory_order_release);
  progressed.notify_all();
}

void GeodeWgpuAdapterDevice::completeWhenQueueDrains(uint64_t ticket) {
  struct WorkDoneState {
    std::shared_ptr<CompletionState> completion;  //!< State independent of adapter lifetime.
    uint64_t ticket = 0;                          //!< Unique range completed by this callback.

    void onWorkDone() { completion->complete(ticket); }
  };
  auto workDoneState = std::make_shared<WorkDoneState>();
  workDoneState->completion = completionState_;
  workDoneState->ticket = ticket;
  notifyWhenSubmittedWorkDone(root_->queue(), workDoneState);
}

gpu::Status GeodeWgpuAdapterDevice::encodeSubmittedCommandBuffer(
    EncodingState& state, std::span<const gpu::Command> commands) {
  if (!state.encoder) {
    return GpuError{GpuErrorType::InvalidState, "wgpu command encoder creation failed"};
  }

  // On any encoding failure, close an open pass before returning so the un-finished command
  // encoder tears down cleanly, then fail closed.
  const auto failEncoding = [&state](gpu::Status error) -> gpu::Status {
    if (state.pass) {
      state.pass.get().end();
      state.pass.reset();
    }
    if (state.computePass) {
      state.computePass.get().end();
      state.computePass.reset();
    }
    return error;
  };

  for (const gpu::Command& command : commands) {
    gpu::Status commandStatus = encodeCommand(state, command);
    if (commandStatus.hasError()) {
      return failEncoding(std::move(commandStatus));
    }
  }

  if (state.pass || state.computePass) {
    // The encoder state machine guarantees passes are ended before finish; fail closed anyway.
    return failEncoding(
        GpuError{GpuErrorType::InvalidState, "submitted command stream left a pass open"});
  }
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::onSubmit(
    uint64_t submissionSerial, std::span<const gpu::SubmittedCommandBuffer> commandBuffers) {
  // The transitional adapter leaves acceptance to wgpu even after the shared root reports loss;
  // its queue can answer asynchronously. Native backends refuse a lost root before recording work.
  // Each submitted buffer gets its own encoder, so the caller's split survives to the queue; they
  // are finished but not submitted until all of them encode, and then handed over together, which
  // keeps the submission ordered and completing once.
  std::vector<ScopedWgpuHandle<wgpu::CommandBuffer>> finished;
  finished.reserve(commandBuffers.size());
  for (const gpu::SubmittedCommandBuffer& commandBuffer : commandBuffers) {
    EncodingState state;
    state.encoder.reset(root_->device().createCommandEncoder());

    if (gpu::Status status = encodeSubmittedCommandBuffer(state, commandBuffer.commands);
        status.hasError()) {
      return status;
    }

    finished.emplace_back(state.encoder.get().finish());
    if (!finished.back()) {
      return GpuError{GpuErrorType::InvalidState, "wgpu command buffer finish failed"};
    }
  }

  std::vector<WGPUCommandBuffer> rawCommandBuffers;
  rawCommandBuffers.reserve(finished.size());
  for (ScopedWgpuHandle<wgpu::CommandBuffer>& commandBuffer : finished) {
    rawCommandBuffers.push_back(static_cast<WGPUCommandBuffer>(commandBuffer.get()));
  }
  completionState_->record(submissionSerial);
  root_->queue().submit(rawCommandBuffers);

  completeWhenQueueDrains(submissionSerial);
  return OkStatus();
}

}  // namespace donner::geode
