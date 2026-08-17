// Exactly one translation unit in the binary must define
// `WEBGPU_CPP_IMPLEMENTATION` before including `<webgpu/webgpu.hpp>`. The
// header ships the body of every C++ wrapper method inside a
// `#ifdef WEBGPU_CPP_IMPLEMENTATION` block; without this define the
// wrapper methods are declared but never defined, and linking fails with
// unresolved `wgpu::Instance::requestAdapter` and friends.
#define WEBGPU_CPP_IMPLEMENTATION
#include "donner/svg/renderer/geode/GeodeDevice.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string_view>
#include <thread>

#include "donner/base/AsyncifySuspendProbe.h"
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif
#include "donner/base/StringUtils.h"
#include "donner/svg/renderer/geode/GeodeCallbackState.h"
#include "donner/svg/renderer/geode/GeodeCheckerboardPipeline.h"
#include "donner/svg/renderer/geode/GeodeFilterEngine.h"
#include "donner/svg/renderer/geode/GeodeGpuWait.h"
#include "donner/svg/renderer/geode/GeodeImagePipeline.h"
#include "donner/svg/renderer/geode/GeodePipeline.h"

#ifdef __EMSCRIPTEN__
// clang-format off: EM_JS contains JavaScript, whose arrow syntax clang-format corrupts.
// Keep this internal import name compact: EM_JS function names survive Closure in editor.js.
EM_JS(void, G, (void* deviceOut, WGPUInstance instance), {
  navigator.gpu.requestAdapter()
    .then((adapter) => adapter.requestDevice())
    .then((device) => WebGPU.importJsDevice(device, instance))
    .catch(() => 1)
    .then((devicePtr) => setTimeout(() => Atomics.store(HEAP32, deviceOut >> 2, devicePtr)));
});
// clang-format on
#endif

namespace donner::geode {

namespace {

#ifndef __EMSCRIPTEN__
/// Error callback wired onto the WebGPU device via
/// `DeviceDescriptor::uncapturedErrorCallbackInfo`. Any driver-level
/// validation errors (missing bindings, bad draw parameters, etc.)
/// surface here.
///
/// The WebGPU C API passes the message as a `WGPUStringView` (pointer +
/// length) rather than a NUL-terminated string, so we use the
/// precision-length form of `printf` to avoid reading past `length`.
void OnUncapturedError(WGPUDevice const* /*device*/, WGPUErrorType type, WGPUStringView message,
                       void* /*userdata1*/, void* /*userdata2*/) {
  std::fprintf(stderr, "[Geode/wgpu-native] Uncaptured error (type=%d): %.*s\n",
               static_cast<int>(type), static_cast<int>(message.length),
               message.data ? message.data : "");
}

/// Device-lost callback wired onto headless devices. Fires at most once per
/// device; `userdata1` carries a retained shared reference to the device's
/// `GeodeDeviceLostState` so a spontaneous callback can outlive the
/// `GeodeDevice` that registered it. A driver-reported loss sets the same
/// flag that bounded-wait timeouts set, so both failure modes surface as one
/// detectable device-lost condition.
void OnDeviceLost(WGPUDevice const* /*device*/, WGPUDeviceLostReason reason, WGPUStringView message,
                  void* userdata1, void* /*userdata2*/) {
  const std::shared_ptr<GeodeDeviceLostState> state =
      takeWgpuCallbackState<GeodeDeviceLostState>(userdata1);
  if (reason == WGPUDeviceLostReason_Destroyed || reason == WGPUDeviceLostReason_InstanceDropped) {
    // Expected teardown paths, not a driver failure.
    return;
  }
  std::fprintf(stderr, "[Geode/wgpu-native] Device lost (reason=%d): %.*s\n",
               static_cast<int>(reason), static_cast<int>(message.length),
               message.data ? message.data : "");
  if (state) {
    state->lost.store(true, std::memory_order_release);
  }
}

wgpu::BackendType RequestedHeadlessBackend() {
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

    std::fprintf(stderr,
                 "[Geode/wgpu-native] Ignoring unsupported WGPU_BACKEND=%.*s; "
                 "using platform default.\n",
                 static_cast<int>(backend.size()), backend.data());
  }

#if defined(__linux__)
  return wgpu::BackendType::Vulkan;
#else
  return wgpu::BackendType::Undefined;
#endif
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

wgpu::Instance CreateHeadlessInstance(wgpu::BackendType backendType) {
  const WGPUInstanceBackend instanceBackends = InstanceBackendsFor(backendType);
  if (instanceBackends != WGPUInstanceBackend_All) {
    wgpu::InstanceExtras instanceExtras = wgpu::Default;
    instanceExtras.backends = instanceBackends;

    wgpu::InstanceDescriptor instanceDesc = wgpu::Default;
    instanceDesc.nextInChain = &instanceExtras.chain;
    return wgpu::createInstance(instanceDesc);
  }
  return wgpu::createInstance();
}
#endif

template <typename Handle>
void DestroyResourceBacking(ScopedWgpuHandle<Handle>& handle) {
  if (handle) {
    handle.get().destroy();
  }
}

}  // namespace

/// PIMPL struct: holds the wgpu::Instance so its lifetime is tied to
/// the GeodeDevice wrapper. Adapter/device/queue handles are stored
/// directly on the outer class. In embedded mode, `instance` is null
/// because the host owns the instance.
struct GeodeDevice::Impl {
  ~Impl() {
    DestroyResourceBacking(dummyPatternTexture);
    DestroyResourceBacking(dummyClipMaskTexture);
    DestroyResourceBacking(identityInstanceTransformBuffer);
    for (auto& [unusedKey, entry] : snapshotReadbackPool) {
      (void)unusedKey;
      DestroyResourceBacking(entry.resources.staging);
      DestroyResourceBacking(entry.resources.readback);
    }
  }

  wgpu::Instance instance;

  // Shared dummies used by every GeoEncoder's bind groups - see
  // comment block on `GeodeDevice::dummyPatternTexture()`. Created
  // once at `CreateHeadless` time so they never count against
  // per-frame `textureCreates` ceilings.
  ScopedWgpuHandle<wgpu::Texture> dummyPatternTexture;
  ScopedWgpuHandle<wgpu::TextureView> dummyPatternTextureView;
  ScopedWgpuHandle<wgpu::Sampler> dummyPatternSampler;
  ScopedWgpuHandle<wgpu::Texture> dummyClipMaskTexture;
  ScopedWgpuHandle<wgpu::TextureView> dummyClipMaskTextureView;
  ScopedWgpuHandle<wgpu::Sampler> dummyClipMaskSampler;

  // 1-element instance-transform buffer bound by every
  // non-instanced solid fill. Uploaded once at CreateHeadless time,
  // never modified. Layout matches the WGSL `InstanceTransform`
  // struct: two vec4f rows carrying the identity affine
  // `{(1,0,0,0), (0,1,0,0)}`.
  ScopedWgpuHandle<wgpu::Buffer> identityInstanceTransformBuffer;

  // Shared render / compute pipelines. Constructed once per GeodeDevice
  // in `initSharedPipelines` - see the public `pipeline()` / ... / `filterEngine()`
  // accessors on GeodeDevice for the "why" behind sharing. These fields
  // are at the bottom of Impl so they destruct before the wgpu::Device
  // at the top of GeodeDevice (reverse-declaration order).
  std::unique_ptr<GeodePipeline> pipeline;
  /// Built lazily on first `checkerboardPipeline()` access - see the header.
  std::unique_ptr<GeodeCheckerboardPipeline> checkerboardPipeline;
  /// Built lazily on first `checkerboardUnderlayPipeline()` access - see the header.
  std::unique_ptr<GeodeCheckerboardPipeline> checkerboardUnderlayPipeline;
  std::unique_ptr<GeodeGradientPipeline> gradientPipeline;
  std::unique_ptr<GeodeImagePipeline> imagePipeline;
  /// Built lazily on first `maskPipeline()` access - see the header.
  std::unique_ptr<GeodeMaskPipeline> maskPipeline;
  std::unique_ptr<GeodeFilterEngine> filterEngine;
  /// Built lazily on first `snapshotReadbackPipeline()` access - see the header.
  std::unique_ptr<GeodeSnapshotReadbackPipeline> snapshotReadbackPipeline;

  /// Size-keyed free pool of GPU snapshot readback resources (staging texture,
  /// view, and map-readable buffer). One entry per (width, height) so repeat
  /// snapshots at the same dimensions allocate nothing. Bounded: entries
  /// carry a last-use tick, and the least-recently-used entry is destroyed
  /// when a release would exceed kMaxSnapshotReadbackPoolEntries, so a
  /// size-churning caller (a window resize walks hundreds of canvas sizes)
  /// cannot accumulate retained staging memory for the device's lifetime.
  struct SnapshotReadbackPoolEntry {
    SnapshotReadbackResources resources;
    uint64_t lastUsedTick = 0;
  };
  std::mutex snapshotReadbackPoolMutex;
  std::map<std::pair<uint32_t, uint32_t>, SnapshotReadbackPoolEntry> snapshotReadbackPool;
  uint64_t snapshotReadbackPoolTick = 0;

  /// Guards the lazy snapshotReadbackPipeline construction: the device is
  /// documented as shareable between the main thread and the async-render
  /// worker, and both can take a first snapshot concurrently. A plain
  /// null-check would let both construct, and the second assignment would
  /// destroy the pipeline the first thread already holds a reference to.
  std::once_flag snapshotReadbackPipelineOnce;
};

/// Distinct snapshot sizes retained by the readback pool: covers the main
/// canvas, the async-render worker, and thumbnail/icon sizes without letting
/// a resize sweep pin one entry per intermediate size.
constexpr size_t kMaxSnapshotReadbackPoolEntries = 4;

namespace {
/// Monotonic source for `GeodeDevice::deviceId()`. Never reused, starts at 1
/// (0 is the "no device" sentinel on a `GeodeResidentSlot`).
std::atomic<uint64_t> g_nextDeviceId{0};
}  // namespace

GeodeDevice::GeodeDevice()
    : impl_(std::make_unique<Impl>()),
      deviceId_(g_nextDeviceId.fetch_add(1, std::memory_order_relaxed) + 1),
      lostState_(std::make_shared<GeodeDeviceLostState>()) {}

GeodeDevice::~GeodeDevice() {
  // Release all resources that were created from the device before releasing the
  // root queue/device/adapter/instance handles. `webgpu.hpp` handles are raw
  // wrappers: their destructors do not release native references.
  //
  // Every teardown wait is bounded. Once the device is lost (driver-reported
  // or a wait deadline expired), all further GPU waits are skipped: waiting
  // on a hung driver can block forever, in the worst case in uninterruptible
  // kernel sleep, and a leak is strictly better than a hung thread.
#ifndef __EMSCRIPTEN__
  if (device_ && queue_) {
    waitForQueueIdle();
  }
#endif
  wgpu::Instance instance;
  if (!external_ && impl_) {
    instance = impl_->instance;
    impl_->instance = wgpu::Instance();
  }
  drainDeferredDestroys();
  impl_.reset();
  if (device_) {
#ifndef __EMSCRIPTEN__
    // Second drain after the pipelines and pooled resources released above;
    // returns immediately if the first wait already declared the device lost.
    waitForQueueIdle();
#endif
  }

  if (external_) {
    queue_ = wgpu::Queue();
    device_ = wgpu::Device();
    adapter_ = wgpu::Adapter();
    return;
  }

  if (isDeviceLost()) {
    // Deliberate leak: destroying or releasing the root handles of a hung
    // device can block inside the driver with no bound. Abandon them; the
    // process has already lost GPU rendering on this device.
    //
    // Post-loss teardown is wait-free, not driver-free: drainDeferredDestroys
    // and the Impl reset above still issued release/destroy calls into the
    // client library for pooled textures, buffers, and pipelines. Those are
    // refcount drops and deferred-destroy marks that do not wait on GPU
    // completion; only the root-handle destroy/release below, which can
    // trigger a blocking device drain, is skipped.
    return;
  }

  ReleaseWgpuHandle(queue_);
  if (device_) {
    device_.destroy();
  }
  ReleaseWgpuHandle(device_);
  ReleaseWgpuHandle(adapter_);
  ReleaseWgpuHandle(instance);
}

bool GeodeDevice::pollSuspending(bool wait) const {
  const ScopedSuspendPoint suspend(SuspendKind::DeviceWait);
  return device_.poll(wait, nullptr);
}

void GeodeDevice::markDeviceLost(const char* reason) const {
  if (!lostState_->lost.exchange(true, std::memory_order_acq_rel)) {
    std::fprintf(stderr, "[Geode] Device declared lost: %s\n", reason ? reason : "(no reason)");
  }
}

GpuWaitResult GeodeDevice::waitForQueueIdle(std::chrono::milliseconds timeout) const {
  if (isDeviceLost()) {
    return GpuWaitResult::DeviceLost;
  }
  if (!device_) {
    return GpuWaitResult::Complete;
  }
#ifdef __EMSCRIPTEN__
  // emdawnwebgpu's poll yields the Asyncify thread for one browser task and
  // its return value does not report queue-idle, so a drain loop keyed on it
  // could spin for the full timeout every call. Keep the single poll-yield
  // this path always performed; browser device hangs surface through the
  // readback map deadline instead.
  (void)timeout;
  pollSuspending(true);
  return GpuWaitResult::Complete;
#else
  const GpuWaitResult result = BoundedGpuWait([this] { return pollSuspending(false); }, timeout);
  if (result == GpuWaitResult::TimedOut) {
    markDeviceLost("GPU queue did not go idle within the bounded wait deadline");
  }
  return result;
#endif
}

void GeodeDevice::recordReadback(bool usedTimedWaitAny, int pollIterations) {
  readbackCount_.fetch_add(1, std::memory_order_relaxed);
  readbackPollIterations_.fetch_add(pollIterations, std::memory_order_relaxed);
  if (usedTimedWaitAny) {
    readbackUsedTimedWaitAny_.store(true, std::memory_order_relaxed);
  }
}

GeodeDevice::ReadbackStats GeodeDevice::consumeReadbackStats() {
  return ReadbackStats{
      .count = readbackCount_.exchange(0, std::memory_order_relaxed),
      .pollIterations = readbackPollIterations_.exchange(0, std::memory_order_relaxed),
      .usedTimedWaitAny = readbackUsedTimedWaitAny_.exchange(false, std::memory_order_relaxed),
  };
}

namespace {
/// Process-wide count of CreateHeadless calls, for tests that pin device
/// sharing. Monotonic; never reset.
std::atomic<int> gHeadlessCreationCount{0};

#ifdef __EMSCRIPTEN__
struct BrowserImportState {
  std::atomic<WGPUDevice> device = nullptr;
};
static_assert(sizeof(BrowserImportState) == sizeof(WGPUDevice));
static_assert(alignof(BrowserImportState) == alignof(WGPUDevice));
static_assert(std::atomic<WGPUDevice>::is_always_lock_free);
#endif
}  // namespace

int GeodeDevice::headlessCreationCountForTesting() {
  return gHeadlessCreationCount.load(std::memory_order_relaxed);
}

std::unique_ptr<GeodeDevice> GeodeDevice::CreateHeadless(wgpu::TextureFormat textureFormat) {
#ifdef __EMSCRIPTEN__
  gHeadlessCreationCount.fetch_add(1, std::memory_order_relaxed);
  auto result = std::unique_ptr<GeodeDevice>(new GeodeDevice());
  result->textureFormat_ = textureFormat;

  const WGPUInstanceFeatureName timedWaitFeature = WGPUInstanceFeatureName_TimedWaitAny;
  WGPUInstanceDescriptor instanceDescriptor = WGPU_INSTANCE_DESCRIPTOR_INIT;
  instanceDescriptor.requiredFeatureCount = 1;
  instanceDescriptor.requiredFeatures = &timedWaitFeature;
  result->impl_->instance = wgpu::Instance(wgpuCreateInstance(&instanceDescriptor));
  if (!result->impl_->instance) {
    std::fprintf(stderr, "[Geode/emscripten] wgpuCreateInstance returned null.\n");
    return nullptr;
  }

  // WebKit cannot drive Emdawn's adapter/device futures through WaitAnyOnly from this transferred
  // renderer pthread. Import one direct Promise chain, then cross a task boundary before the C
  // continuation initializes pipelines. The adapter stays JavaScript-only because Emdawn accepts
  // the instance as an imported device's future parent. Snapshot map futures still use
  // TimedWaitAny below.
  BrowserImportState state;
  WGPUDevice importedDevice = nullptr;
  G(&state.device, result->impl_->instance);
  while ((importedDevice = state.device.load(std::memory_order_acquire)) == nullptr) {
    emscripten_sleep(1);
  }
  if (reinterpret_cast<std::uintptr_t>(importedDevice) == 1) {
    std::fprintf(stderr, "[Geode/emscripten] Browser WebGPU device request failed.\n");
    return nullptr;
  }
  result->device_ = wgpu::Device(importedDevice);
  result->queue_ = result->device_.getQueue();
  if (!result->queue_) {
    std::fprintf(stderr, "[Geode/emscripten] Browser WebGPU device returned no queue.\n");
    return nullptr;
  }

  result->initSharedResources();
  result->initSharedPipelines();
  return result;
#else
  gHeadlessCreationCount.fetch_add(1, std::memory_order_relaxed);
  auto result = std::unique_ptr<GeodeDevice>(new GeodeDevice());
  result->textureFormat_ = textureFormat;

  // 1. Create the WebGPU instance. wgpu-native's `wgpuCreateInstance`
  //    is synchronous and never blocks on I/O; the returned handle is
  //    the root of the object graph.
  const wgpu::BackendType headlessBackend = RequestedHeadlessBackend();
  result->impl_->instance = CreateHeadlessInstance(headlessBackend);
  if (!result->impl_->instance) {
    std::fprintf(stderr, "[Geode/wgpu-native] wgpuCreateInstance returned null\n");
    return nullptr;
  }

  // 2. Request a GPU adapter. The synchronous form in webgpu-cpp
  //    internally calls the async `wgpuInstanceRequestAdapter` with a
  //    lambda that parks the result on the stack - wgpu-native invokes
  //    the callback before returning from the request, so the sync
  //    form is safe on native targets (Emscripten loops on
  //    `emscripten_sleep` instead).
  wgpu::RequestAdapterOptions adapterOptions = {};
  adapterOptions.backendType = headlessBackend;
  adapterOptions.forceFallbackAdapter = wgpuForceFallbackAdapterRequested();

  // Bounded retry around adapter acquisition, mirroring the device-init
  // retry below (#880). Under heavy parallel load the adapter request can
  // transiently fail at the adapter level - wgpu-native logs
  // \"Could not get WebGPU adapter: Validation Error / No suitable adapter
  // found\" and requestAdapter returns null - before requestDevice is even
  // reached. This is the same driver-side race under contention; the
  // adapter re-request after a short backoff succeeds. A permanently
  // missing adapter (no GPU, wrong backend) simply exhausts the retries and
  // returns nullptr, which RendererGeode handles as no-op mode. Every retry
  // is logged so the flake stays observable in test logs.
  constexpr int kMaxAdapterRetries = 3;
  constexpr int kAdapterBackoffMs[kMaxAdapterRetries] = {50, 200, 800};
  for (int attempt = 0;; ++attempt) {
    result->adapter_ = result->impl_->instance.requestAdapter(adapterOptions);
    if (result->adapter_) {
      break;  // Adapter acquired.
    }

    std::fprintf(stderr, "[Geode/wgpu-native] No WebGPU adapter available.\n");
    if (attempt >= kMaxAdapterRetries) {
      std::fprintf(stderr, "[Geode/wgpu-native] Giving up after %d adapter-acquisition retries.\n",
                   kMaxAdapterRetries);
      return nullptr;
    }
    const int backoffMs = kAdapterBackoffMs[attempt];
    std::fprintf(stderr,
                 "[Geode/wgpu-native] Transient adapter-acquisition failure under parallel "
                 "load; retrying (attempt %d of %d) after %d ms.\n",
                 attempt + 1, kMaxAdapterRetries, backoffMs);
    std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
  }

  // Log adapter selection so it is obvious at a glance whether we landed
  // on a discrete GPU / integrated GPU / software rasterizer, and which
  // native backend (Vulkan / Metal / D3D12 / ...) is driving it.
  //
  // Under Emscripten the wgpuAdapterGetInfo / WGPUAdapterInfo struct
  // layout may differ from wgpu-native v24 (WGPUStringView fields vs raw
  // char*). Skip the native-specific logging for now - the browser's
  // DevTools console already surfaces adapter info.
#ifndef __EMSCRIPTEN__
  {
    WGPUAdapterInfo info = {};
    if (wgpuAdapterGetInfo(result->adapter_, &info) == WGPUStatus_Success) {
      auto sv = [](const WGPUStringView& s) {
        return std::string_view{s.data ? s.data : "", s.data ? s.length : 0};
      };
      const char* backend = "?";
      switch (info.backendType) {
        case WGPUBackendType_Vulkan: backend = "Vulkan"; break;
        case WGPUBackendType_Metal: backend = "Metal"; break;
        case WGPUBackendType_D3D12: backend = "D3D12"; break;
        case WGPUBackendType_D3D11: backend = "D3D11"; break;
        case WGPUBackendType_OpenGL: backend = "OpenGL"; break;
        case WGPUBackendType_OpenGLES: backend = "OpenGLES"; break;
        case WGPUBackendType_WebGPU: backend = "WebGPU"; break;
        case WGPUBackendType_Null: backend = "Null"; break;
        default: break;
      }
      const char* type = "?";
      switch (info.adapterType) {
        case WGPUAdapterType_DiscreteGPU: type = "DiscreteGPU"; break;
        case WGPUAdapterType_IntegratedGPU: type = "IntegratedGPU"; break;
        case WGPUAdapterType_CPU: type = "CPU"; break;
        case WGPUAdapterType_Unknown: type = "Unknown"; break;
        default: break;
      }
      const auto vendor = sv(info.vendor);
      const auto device = sv(info.device);
      const auto arch = sv(info.architecture);
      std::fprintf(stderr,
                   "[Geode/wgpu-native] Adapter: %.*s %.*s (%.*s) "
                   "backend=%s type=%s vendorID=0x%04x deviceID=0x%04x\n",
                   static_cast<int>(vendor.size()), vendor.data(), static_cast<int>(device.size()),
                   device.data(), static_cast<int>(arch.size()), arch.data(), backend, type,
                   info.vendorID, info.deviceID);

      // Record whether we landed on a Vulkan backend (Intel Arc hardware or
      // Mesa lavapipe software). GeodeFilterEngine uses this to serialize its
      // per-pass compute submits, working around a nondeterministic
      // cross-submit texture-visibility race that only Vulkan exposes.
      // If wgpuAdapterGetInfo fails on a real Vulkan device the fix silently
      // disables (accepted residual risk).
      result->isVulkan_ = (info.backendType == WGPUBackendType_Vulkan);

      wgpuAdapterInfoFreeMembers(info);
    }
  }
#endif

  // 3. Create the device. Error diagnostics are wired via
  //    `uncapturedErrorCallbackInfo` on the descriptor - the callback
  //    stays valid for the device's lifetime.
  //
  wgpu::DeviceDescriptor deviceDesc = {};
  deviceDesc.label = wgpu::StringView{std::string_view{"GeodeDevice"}};
  deviceDesc.deviceLostCallbackInfo.mode = wgpu::CallbackMode::AllowSpontaneous;
  deviceDesc.deviceLostCallbackInfo.callback = OnDeviceLost;
  deviceDesc.deviceLostCallbackInfo.userdata2 = nullptr;
  deviceDesc.uncapturedErrorCallbackInfo.callback = OnUncapturedError;
  deviceDesc.uncapturedErrorCallbackInfo.userdata1 = nullptr;
  deviceDesc.uncapturedErrorCallbackInfo.userdata2 = nullptr;

  // Bounded retry around device creation only.
  //
  // Under heavy parallel load the Intel Arc (ANV) Vulkan driver
  // intermittently fails device initialization with a transient
  // "Validation Error" (empirically around 1 in 80 device creations). This
  // is a driver-side device-init race under contention, not a
  // deterministic capability problem: the adapter was acquired
  // successfully just above, and re-requesting the device after a short
  // backoff succeeds. Retry a bounded number of times with exponential
  // backoff, logging every retry so the flake stays observable in test
  // logs rather than being silently absorbed.
  //
  // Only a null device return from requestDevice is retried. The
  // deterministic failures (null instance, no adapter for the requested
  // backend) already returned above and are never retried; a device lost
  // after successful creation is out of scope here.
  constexpr int kMaxDeviceInitRetries = 3;
  constexpr int kDeviceInitBackoffMs[kMaxDeviceInitRetries] = {50, 200, 800};
  for (int attempt = 0;; ++attempt) {
    // A fresh retained lost-state reference per attempt: each successfully
    // created device eventually consumes its userdata exactly once via
    // OnDeviceLost (including the Destroyed-at-teardown delivery). An attempt
    // that returns a null device strands at most one small retained block,
    // bounded by the retry count.
    deviceDesc.deviceLostCallbackInfo.userdata1 = retainWgpuCallbackState(result->lostState_);
    result->device_ = result->adapter_.requestDevice(deviceDesc);
    if (result->device_) {
      break;  // Device created successfully.
    }

    std::fprintf(stderr, "[Geode/wgpu-native] Failed to create device.\n");
    if (attempt >= kMaxDeviceInitRetries) {
      std::fprintf(stderr, "[Geode/wgpu-native] Giving up after %d device-creation retries.\n",
                   kMaxDeviceInitRetries);
      return nullptr;
    }
    const int backoffMs = kDeviceInitBackoffMs[attempt];
    std::fprintf(stderr,
                 "[Geode/wgpu-native] Transient device-init failure under parallel "
                 "load; retrying (attempt %d of %d) after %d ms.\n",
                 attempt + 1, kMaxDeviceInitRetries, backoffMs);
    std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
  }

  // 4. Grab the default queue.
  result->queue_ = result->device_.getQueue();
  if (!result->queue_) {
    std::fprintf(stderr, "[Geode/wgpu-native] Failed to get queue.\n");
    return nullptr;
  }

  result->initSharedResources();
  result->initSharedPipelines();

  return result;
#endif
}

const wgpu::Texture& GeodeDevice::dummyPatternTexture() const {
  return impl_->dummyPatternTexture.get();
}
const wgpu::TextureView& GeodeDevice::dummyPatternTextureView() const {
  return impl_->dummyPatternTextureView.get();
}
const wgpu::Sampler& GeodeDevice::dummyPatternSampler() const {
  return impl_->dummyPatternSampler.get();
}
const wgpu::Texture& GeodeDevice::dummyClipMaskTexture() const {
  return impl_->dummyClipMaskTexture.get();
}
const wgpu::TextureView& GeodeDevice::dummyClipMaskTextureView() const {
  return impl_->dummyClipMaskTextureView.get();
}
const wgpu::Sampler& GeodeDevice::dummyClipMaskSampler() const {
  return impl_->dummyClipMaskSampler.get();
}
const wgpu::Buffer& GeodeDevice::identityInstanceTransformBuffer() const {
  return impl_->identityInstanceTransformBuffer.get();
}

GeodePipeline& GeodeDevice::pipeline() const {
  return *impl_->pipeline;
}
GeodeGradientPipeline& GeodeDevice::gradientPipeline() const {
  return *impl_->gradientPipeline;
}
GeodeImagePipeline& GeodeDevice::imagePipeline() const {
  return *impl_->imagePipeline;
}
GeodeMaskPipeline& GeodeDevice::maskPipeline() const {
  if (!impl_->maskPipeline) {
    // Lazy: most documents never hit the clip-path mask pass, and
    // production WASM callers that never need it should not pay the
    // pipeline-compile cost at startup.
    impl_->maskPipeline = std::make_unique<GeodeMaskPipeline>(device_);
  }
  return *impl_->maskPipeline;
}
GeodeFilterEngine& GeodeDevice::filterEngine() const {
  return *impl_->filterEngine;
}
GeodeSnapshotReadbackPipeline& GeodeDevice::snapshotReadbackPipeline() const {
  // Lazy: only snapshot readback consumes this pipeline, so renderers that
  // never call takeSnapshot() avoid the compile cost. call_once, not a plain
  // null-check: see Impl::snapshotReadbackPipelineOnce.
  std::call_once(impl_->snapshotReadbackPipelineOnce, [this] {
    impl_->snapshotReadbackPipeline = std::make_unique<GeodeSnapshotReadbackPipeline>(device_);
  });
  return *impl_->snapshotReadbackPipeline;
}

SnapshotReadbackResources GeodeDevice::acquireSnapshotReadbackResources(uint32_t width,
                                                                        uint32_t height) {
  const std::pair<uint32_t, uint32_t> key(width, height);
  {
    std::lock_guard<std::mutex> lock(impl_->snapshotReadbackPoolMutex);
    auto it = impl_->snapshotReadbackPool.find(key);
    if (it != impl_->snapshotReadbackPool.end()) {
      SnapshotReadbackResources result = std::move(it->second.resources);
      impl_->snapshotReadbackPool.erase(it);
      return result;
    }
  }

  // First use at this size: allocate the staging texture, its view, and the
  // map-readable readback buffer. Bytes-per-row must be 256-aligned per the
  // WebGPU texture-to-buffer copy rules.
  SnapshotReadbackResources resources;
  resources.width = width;
  resources.height = height;

  wgpu::TextureDescriptor td = {};
  td.label = wgpuLabel("RendererGeodeReadbackStaging");
  td.size = {width, height, 1};
  td.format = wgpu::TextureFormat::RGBA8Unorm;
  td.usage = wgpu::TextureUsage::StorageBinding | wgpu::TextureUsage::CopySrc;
  td.mipLevelCount = 1;
  td.sampleCount = 1;
  td.dimension = wgpu::TextureDimension::_2D;
  resources.staging.reset(device_.createTexture(td));
  if (resources.staging) {
    resources.stagingView.reset(resources.staging.get().createView());
    countTexture();
  }

  const uint32_t bytesPerRow = AlignReadbackBytesPerRow(width * 4u);
  wgpu::BufferDescriptor bd = {};
  bd.label = wgpuLabel("RendererGeodeReadback");
  bd.size = static_cast<uint64_t>(bytesPerRow) * static_cast<uint64_t>(height);
  bd.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
  resources.readback.reset(device_.createBuffer(bd));
  if (resources.readback) {
    countBuffer();
  }

  if (resources.empty()) {
    // Partial allocation failure: release what was created without pooling it.
    resources = SnapshotReadbackResources{};
  }
  return resources;
}

void GeodeDevice::releaseSnapshotReadbackResources(SnapshotReadbackResources resources) {
  if (resources.empty()) {
    return;
  }
  const std::pair<uint32_t, uint32_t> key(resources.width, resources.height);
  std::lock_guard<std::mutex> lock(impl_->snapshotReadbackPoolMutex);
  Impl::SnapshotReadbackPoolEntry entry;
  entry.resources = std::move(resources);
  entry.lastUsedTick = ++impl_->snapshotReadbackPoolTick;
  impl_->snapshotReadbackPool.insert_or_assign(key, std::move(entry));

  while (impl_->snapshotReadbackPool.size() > kMaxSnapshotReadbackPoolEntries) {
    auto lruIt = impl_->snapshotReadbackPool.begin();
    for (auto it = std::next(impl_->snapshotReadbackPool.begin());
         it != impl_->snapshotReadbackPool.end(); ++it) {
      if (it->second.lastUsedTick < lruIt->second.lastUsedTick) {
        lruIt = it;
      }
    }
    // Pooled entries are idle by construction (release happens only after
    // the readback unmaps), so their backings can be destroyed eagerly
    // instead of deferred to a frame boundary.
    DestroyResourceBacking(lruIt->second.resources.staging);
    DestroyResourceBacking(lruIt->second.resources.readback);
    impl_->snapshotReadbackPool.erase(lruIt);
  }
}
const wgpu::Instance& GeodeDevice::instance() const {
  return impl_->instance;
}
GeodeCheckerboardPipeline& GeodeDevice::checkerboardPipeline() const {
  if (!impl_->checkerboardPipeline) {
    // Lazy: only the editor's direct framebuffer presentation draws the
    // checkerboard; other consumers should not pay the pipeline-compile cost
    // at startup.
    impl_->checkerboardPipeline = std::make_unique<GeodeCheckerboardPipeline>(
        device_, textureFormat_, GeodeCheckerboardPipeline::BlendMode::Replace);
  }
  return *impl_->checkerboardPipeline;
}
GeodeCheckerboardPipeline& GeodeDevice::checkerboardUnderlayPipeline() const {
  if (!impl_->checkerboardUnderlayPipeline) {
    // Lazy for the same reason as `checkerboardPipeline()`, and separate from
    // it because a consumer normally draws through exactly one of the two:
    // before the document pixels (replace) or after them (destination-over).
    impl_->checkerboardUnderlayPipeline = std::make_unique<GeodeCheckerboardPipeline>(
        device_, textureFormat_, GeodeCheckerboardPipeline::BlendMode::DestinationOver);
  }
  return *impl_->checkerboardUnderlayPipeline;
}

std::unique_ptr<GeodeDevice> GeodeDevice::CreateFromExternal(const GeodeEmbedConfig& config) {
  if (!config.device || !config.queue) {
    std::fprintf(stderr, "[Geode] CreateFromExternal: null device or queue in config\n");
    return nullptr;
  }

  auto result = std::unique_ptr<GeodeDevice>(new GeodeDevice());
  result->external_ = true;
  if (config.lostState) {
    // Share the host's device-lost flag so a loss reported through the
    // host's device-lost callback and a bounded-wait timeout inside Geode
    // surface as the same isDeviceLost() condition.
    result->lostState_ = config.lostState;
  }
  result->device_ = config.device;
  result->queue_ = config.queue;
  result->textureFormat_ = config.textureFormat;
  // The raw wrapper does not release on destruction. Keep the host instance
  // available for event dispatch without taking ownership of it.
  result->impl_->instance = config.instance;

  // Preserve the host-provided adapter for callers that need adapter metadata.
  if (config.adapter) {
    result->adapter_ = config.adapter;

    // Detect a Vulkan backend on the embedder-supplied adapter the same way
    // CreateHeadless() does, so the filter-engine inter-pass serialization
    // (see isVulkan()) also protects external Vulkan devices.
#ifndef __EMSCRIPTEN__
    {
      WGPUAdapterInfo info = {};
      if (wgpuAdapterGetInfo(result->adapter_, &info) == WGPUStatus_Success) {
        // If wgpuAdapterGetInfo fails on a real Vulkan device the fix
        // silently disables (accepted residual risk).
        result->isVulkan_ = (info.backendType == WGPUBackendType_Vulkan);
        wgpuAdapterInfoFreeMembers(info);
      }
    }
#endif
  }

  result->supportsTimestamps_ = config.device.hasFeature(wgpu::FeatureName::TimestampQuery);

  result->initSharedResources();
  result->initSharedPipelines();

  return result;
}

void GeodeDevice::initSharedResources() {
  // Shared dummy textures / samplers used by every GeoEncoder. These are 1x1
  // identity fills for the pattern and clip-mask bind slots when the current
  // draw doesn't actually use them. Created before any setCounters() call so
  // the allocations never count against per-frame ceilings.
  {
    wgpu::TextureDescriptor td = {};
    td.label = wgpu::StringView{std::string_view{"GeodeDeviceDummyPattern"}};
    td.size = {1u, 1u, 1u};
    td.format = wgpu::TextureFormat::RGBA8Unorm;
    td.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    td.mipLevelCount = 1;
    td.sampleCount = 1;
    td.dimension = wgpu::TextureDimension::_2D;
    impl_->dummyPatternTexture.reset(device_.createTexture(td));

    const uint8_t pixel[4] = {0, 0, 0, 255};
    wgpu::TexelCopyTextureInfo dst = {};
    dst.texture = impl_->dummyPatternTexture.get();
    wgpu::TexelCopyBufferLayout layout = {};
    layout.bytesPerRow = 4;
    layout.rowsPerImage = 1;
    wgpu::Extent3D extent = {1u, 1u, 1u};
    queue_.writeTexture(dst, pixel, sizeof(pixel), layout, extent);
    impl_->dummyPatternTextureView.reset(impl_->dummyPatternTexture.get().createView());

    wgpu::SamplerDescriptor sd{wgpu::Default};
    sd.label = wgpu::StringView{std::string_view{"GeodeDeviceDummyPatternSampler"}};
    sd.addressModeU = wgpu::AddressMode::Repeat;
    sd.addressModeV = wgpu::AddressMode::Repeat;
    sd.minFilter = wgpu::FilterMode::Linear;
    sd.magFilter = wgpu::FilterMode::Linear;
    sd.maxAnisotropy = 1;
    impl_->dummyPatternSampler.reset(device_.createSampler(sd));
  }

  {
    wgpu::TextureDescriptor md = {};
    md.label = wgpu::StringView{std::string_view{"GeodeDeviceDummyClipMask"}};
    md.size = {1u, 1u, 1u};
    md.format = wgpu::TextureFormat::RGBA8Unorm;
    md.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    md.mipLevelCount = 1;
    md.sampleCount = 1;
    md.dimension = wgpu::TextureDimension::_2D;
    impl_->dummyClipMaskTexture.reset(device_.createTexture(md));

    const uint8_t mpixel[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    wgpu::TexelCopyTextureInfo mdst = {};
    mdst.texture = impl_->dummyClipMaskTexture.get();
    wgpu::TexelCopyBufferLayout mlayout = {};
    mlayout.bytesPerRow = 4;
    mlayout.rowsPerImage = 1;
    wgpu::Extent3D mextent = {1u, 1u, 1u};
    queue_.writeTexture(mdst, mpixel, sizeof(mpixel), mlayout, mextent);
    impl_->dummyClipMaskTextureView.reset(impl_->dummyClipMaskTexture.get().createView());

    wgpu::SamplerDescriptor msd{wgpu::Default};
    msd.label = wgpu::StringView{std::string_view{"GeodeDeviceDummyClipMaskSampler"}};
    msd.addressModeU = wgpu::AddressMode::ClampToEdge;
    msd.addressModeV = wgpu::AddressMode::ClampToEdge;
    msd.minFilter = wgpu::FilterMode::Linear;
    msd.magFilter = wgpu::FilterMode::Linear;
    msd.maxAnisotropy = 1;
    impl_->dummyClipMaskSampler.reset(device_.createSampler(msd));
  }

  {
    const float identity[8] = {
        1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
    };
    wgpu::BufferDescriptor bd = {};
    bd.label = wgpu::StringView{std::string_view{"GeodeDeviceIdentityInstanceTransform"}};
    bd.size = sizeof(identity);
    bd.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
    impl_->identityInstanceTransformBuffer.reset(device_.createBuffer(bd));
    queue_.writeBuffer(impl_->identityInstanceTransformBuffer.get(), 0, identity, sizeof(identity));
  }
}

void GeodeDevice::initSharedPipelines() {
  // Requires device_ / queue_ / textureFormat_ to be fully populated.
  // `CreateHeadless` and `CreateFromExternal` both call this as the final step.
  const wgpu::TextureFormat fmt = textureFormat_;

  impl_->pipeline = std::make_unique<GeodePipeline>(device_, fmt);
  impl_->gradientPipeline = std::make_unique<GeodeGradientPipeline>(device_, fmt);
  impl_->imagePipeline = std::make_unique<GeodeImagePipeline>(device_, fmt);
  // Mask pipeline is built on first `maskPipeline()` access - see header.
  impl_->filterEngine = std::make_unique<GeodeFilterEngine>(*this, /*verbose=*/false);
}

void GeodeDevice::deferDestroy(wgpu::Buffer buffer) {
  if (buffer) {
    pendingBuffers_.push_back(ScopedWgpuHandle<wgpu::Buffer>(std::move(buffer)));
  }
}

void GeodeDevice::deferDestroy(wgpu::Texture texture) {
  if (texture) {
    pendingTextures_.push_back(ScopedWgpuHandle<wgpu::Texture>(std::move(texture)));
  }
}

void GeodeDevice::drainDeferredDestroys() {
  pendingBuffers_.clear();
  pendingTextures_.clear();
}

}  // namespace donner::geode
