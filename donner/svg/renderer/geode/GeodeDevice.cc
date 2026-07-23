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
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string_view>
#include <thread>
#include <utility>

#include "donner/base/StringUtils.h"
#include "donner/base/Utils.h"
#include "donner/svg/renderer/geode/GeodeCallbackState.h"
#include "donner/svg/renderer/geode/GeodeCheckerboardPipeline.h"
#include "donner/svg/renderer/geode/GeodeFilterEngine.h"
#include "donner/svg/renderer/geode/GeodeImagePipeline.h"
#include "donner/svg/renderer/geode/GeodePipeline.h"
#include "donner/svg/renderer/geode/GeodeWgpuAdapterDevice.h"

namespace donner::geode {

namespace {

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

void OnDeviceLost(WGPUDevice const* /*device*/, WGPUDeviceLostReason reason, WGPUStringView message,
                  void* /*userdata1*/, void* /*userdata2*/) {
  if (reason == WGPUDeviceLostReason_Destroyed || reason == WGPUDeviceLostReason_InstanceDropped) {
    return;
  }
  std::fprintf(stderr, "[Geode/wgpu-native] Device lost (reason=%d): %.*s\n",
               static_cast<int>(reason), static_cast<int>(message.length),
               message.data ? message.data : "");
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

#ifndef __EMSCRIPTEN__
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
#endif

wgpu::Instance CreateHeadlessInstance(wgpu::BackendType backendType) {
#ifndef __EMSCRIPTEN__
  const WGPUInstanceBackend instanceBackends = InstanceBackendsFor(backendType);
  if (instanceBackends != WGPUInstanceBackend_All) {
    wgpu::InstanceExtras instanceExtras = wgpu::Default;
    instanceExtras.backends = instanceBackends;

    wgpu::InstanceDescriptor instanceDesc = wgpu::Default;
    instanceDesc.nextInChain = &instanceExtras.chain;
    return wgpu::createInstance(instanceDesc);
  }
  return wgpu::createInstance();
#else
  (void)backendType;
  const WGPUInstanceFeatureName timedWaitFeature = WGPUInstanceFeatureName_TimedWaitAny;
  wgpu::InstanceDescriptor descriptor{wgpu::Default};
  descriptor.requiredFeatureCount = 1;
  descriptor.requiredFeatures = &timedWaitFeature;
  return wgpu::createInstance(descriptor);
#endif
}

void WaitForSubmittedWork(const wgpu::Device& device, const wgpu::Queue& queue) {
  if (!device || !queue) {
    return;
  }

  struct WorkDoneState {
    std::atomic<bool> done = false;

    /// Callback hook for `notifyWhenSubmittedWorkDone`.
    void onWorkDone() { done.store(true, std::memory_order_release); }
  };
  auto state = std::make_shared<WorkDoneState>();

  notifyWhenSubmittedWorkDone(queue, state);
  for (int pollIter = 0; !state->done.load(std::memory_order_acquire) && pollIter < 2000;
       ++pollIter) {
    device.poll(true, nullptr);
  }
}

}  // namespace

/// PIMPL struct: holds the wgpu::Instance so its lifetime is tied to
/// the GeodeDevice wrapper. Adapter/device/queue handles are stored
/// directly on the outer class. In embedded mode, `instance` is null
/// because the host owns the instance.
struct GeodeDevice::Impl {
  wgpu::Instance instance;

  // TEMPORARY design-0053 Phase 1 adapter (see GeodeWgpuAdapterDevice.h for the removal
  // gates). Declared ABOVE the shared dummies and the pipelines: everything below holds
  // donner::gpu RAII handles whose destructors release through the adapter, so the adapter
  // must destruct after them (reverse-declaration order).
  std::unique_ptr<GeodeWgpuAdapterDevice> adapterDevice;

  // Shared dummies used by every GeoEncoder's bind groups - see the comment block on
  // `GeodeDevice::dummyPatternTextureView()`. donner::gpu handles created through the adapter
  // (via `GeodeSharedGpuResources::Create`) once at device-construction time, before counters
  // are installed, so they never count against per-frame `textureCreates` ceilings. Declared
  // BELOW the adapter so they release while the adapter is alive.
  GeodeSharedGpuResources sharedResources;

  /// The donner::gpu recording context handed to GeoEncoder / GeodeTextureEncoder. Wired in
  /// `initSharedPipelines` after the adapter and dummies exist.
  GeodeGpuContext gpuContext;

  // Shared render / compute pipelines. Constructed once per GeodeDevice
  // in `initSharedPipelines` - see the public `pipeline()` / ... / `filterEngine()`
  // accessors on GeodeDevice for the "why" behind sharing. These fields
  // are at the bottom of Impl so they destruct before the wgpu::Device
  // at the top of GeodeDevice (reverse-declaration order).
  std::unique_ptr<GeodePipeline> pipeline;
  /// Built lazily on first `checkerboardPipeline()` access - see the header.
  std::unique_ptr<GeodeCheckerboardPipeline> checkerboardPipeline;
  std::unique_ptr<GeodeGradientPipeline> gradientPipeline;
  std::unique_ptr<GeodeImagePipeline> imagePipeline;
  /// Built lazily on first `maskPipeline()` access - see the header.
  std::unique_ptr<GeodeMaskPipeline> maskPipeline;
  std::unique_ptr<GeodeFilterEngine> filterEngine;
};

namespace {
/// Monotonic source for `GeodeDevice::deviceId()`. Never reused, starts at 1
/// (0 is the "no device" sentinel on a `GeodeResidentSlot`).
std::atomic<uint64_t> g_nextDeviceId{0};
}  // namespace

GeodeDevice::GeodeDevice()
    : impl_(std::make_unique<Impl>()),
      deviceId_(g_nextDeviceId.fetch_add(1, std::memory_order_relaxed) + 1) {}
GeodeDevice::~GeodeDevice() {
  // Release all resources that were created from the device before releasing the
  // root queue/device/adapter/instance handles. `webgpu.hpp` handles are raw
  // wrappers: their destructors do not release native references.
  WaitForSubmittedWork(device_, queue_);
  wgpu::Instance instance;
  if (!external_ && impl_) {
    instance = impl_->instance;
    impl_->instance = wgpu::Instance();
  }
  drainDeferredDestroys();
  impl_.reset();
  if (device_) {
    device_.poll(true, nullptr);
    WaitForSubmittedWork(device_, queue_);
  }

  if (external_) {
    queue_ = wgpu::Queue();
    device_ = wgpu::Device();
    adapter_ = wgpu::Adapter();
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

namespace {
/// Process-wide count of CreateHeadless calls, for tests that pin device
/// sharing. Monotonic; never reset.
std::atomic<int> gHeadlessCreationCount{0};
}  // namespace

int GeodeDevice::headlessCreationCountForTesting() {
  return gHeadlessCreationCount.load(std::memory_order_relaxed);
}

std::unique_ptr<GeodeDevice> GeodeDevice::CreateHeadless() {
  gHeadlessCreationCount.fetch_add(1, std::memory_order_relaxed);
  auto result = std::unique_ptr<GeodeDevice>(new GeodeDevice());

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
#else
  std::fprintf(stderr, "[Geode/emscripten] WebGPU adapter acquired (browser-managed).\n");
#endif

  // 3. Create the device. Error diagnostics are wired via
  //    `uncapturedErrorCallbackInfo` on the descriptor - the callback
  //    stays valid for the device's lifetime.
  //
  wgpu::DeviceDescriptor deviceDesc = {};
  deviceDesc.label = wgpu::StringView{std::string_view{"GeodeDevice"}};
  deviceDesc.deviceLostCallbackInfo.mode = wgpu::CallbackMode::AllowSpontaneous;
  deviceDesc.deviceLostCallbackInfo.callback = OnDeviceLost;
  deviceDesc.deviceLostCallbackInfo.userdata1 = nullptr;
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
}

const gpu::TextureView& GeodeDevice::dummyPatternTextureView() const {
  return impl_->sharedResources.dummyPatternTextureView;
}
const gpu::Sampler& GeodeDevice::dummyPatternSampler() const {
  return impl_->sharedResources.dummyPatternSampler;
}
const gpu::TextureView& GeodeDevice::dummyClipMaskTextureView() const {
  return impl_->sharedResources.dummyClipMaskTextureView;
}
const gpu::Sampler& GeodeDevice::dummyClipMaskSampler() const {
  return impl_->sharedResources.dummyClipMaskSampler;
}
const gpu::Buffer& GeodeDevice::identityInstanceTransformBuffer() const {
  return impl_->sharedResources.identityInstanceTransformBuffer;
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
    impl_->maskPipeline = std::make_unique<GeodeMaskPipeline>(*impl_->adapterDevice);
  }
  return *impl_->maskPipeline;
}

GeodeWgpuAdapterDevice& GeodeDevice::adapterDevice() const {
  return *impl_->adapterDevice;
}
const GeodeGpuContext& GeodeDevice::gpuContext() const {
  return impl_->gpuContext;
}
GeodeFilterEngine& GeodeDevice::filterEngine() const {
  return *impl_->filterEngine;
}
const wgpu::Instance& GeodeDevice::instance() const {
  return impl_->instance;
}
GeodeCheckerboardPipeline& GeodeDevice::checkerboardPipeline() const {
  if (!impl_->checkerboardPipeline) {
    // Lazy: only the editor's direct framebuffer presentation draws the
    // checkerboard underlay; other consumers should not pay the
    // pipeline-compile cost at startup.
    impl_->checkerboardPipeline =
        std::make_unique<GeodeCheckerboardPipeline>(device_, textureFormat_);
  }
  return *impl_->checkerboardPipeline;
}

std::unique_ptr<GeodeDevice> GeodeDevice::CreateFromExternal(const GeodeEmbedConfig& config) {
  if (!config.device || !config.queue) {
    std::fprintf(stderr, "[Geode] CreateFromExternal: null device or queue in config\n");
    return nullptr;
  }

  auto result = std::unique_ptr<GeodeDevice>(new GeodeDevice());
  result->external_ = true;
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

gpu::Result<GeodeSharedGpuResources> GeodeSharedGpuResources::Create(gpu::Device& gpuDevice) {
  GeodeSharedGpuResources resources;

  // Shared dummy textures / samplers used by every GeoEncoder. These are 1x1
  // identity fills for the pattern and clip-mask bind slots when the current
  // draw doesn't actually use them.
  //
  // The 1x1 uploads pass bytesPerRow=256 with 4 bytes of pixel data: the runtime requires
  // 256-aligned rows, and validates that the data covers `offset + 0 * bytesPerRow +
  // rowBytes(4)` bytes, so the short final row is accepted.
  {
    gpu::Result<gpu::Texture> texture = gpuDevice.createTexture(gpu::TextureDescriptor{
        "GeodeDeviceDummyPattern", gpu::Extent2d{1, 1}, gpu::TextureFormat::RGBA8Unorm,
        gpu::TextureUsage::Sampled | gpu::TextureUsage::CopyDst});
    if (texture.hasError()) {
      return std::move(texture).error();
    }
    resources.dummyPatternTexture = std::move(texture).result();

    const uint8_t pixel[4] = {0, 0, 0, 255};
    gpu::Status writeStatus =
        gpuDevice.writeTexture(resources.dummyPatternTexture, std::span<const uint8_t>(pixel, 4),
                               gpu::TexelCopyBufferLayout{0, 256, 1}, gpu::Extent2d{1, 1});
    if (writeStatus.hasError()) {
      return std::move(writeStatus).error();
    }

    gpu::Result<gpu::TextureView> view = gpuDevice.createTextureView(
        resources.dummyPatternTexture, gpu::TextureViewDescriptor{"GeodeDeviceDummyPatternView"});
    if (view.hasError()) {
      return std::move(view).error();
    }
    resources.dummyPatternTextureView = std::move(view).result();

    gpu::Result<gpu::Sampler> sampler = gpuDevice.createSampler(gpu::SamplerDescriptor{
        "GeodeDeviceDummyPatternSampler", gpu::FilterMode::Linear, gpu::FilterMode::Linear,
        gpu::AddressMode::Repeat, gpu::AddressMode::Repeat});
    if (sampler.hasError()) {
      return std::move(sampler).error();
    }
    resources.dummyPatternSampler = std::move(sampler).result();
  }

  {
    gpu::Result<gpu::Texture> texture = gpuDevice.createTexture(gpu::TextureDescriptor{
        "GeodeDeviceDummyClipMask", gpu::Extent2d{1, 1}, gpu::TextureFormat::RGBA8Unorm,
        gpu::TextureUsage::Sampled | gpu::TextureUsage::CopyDst});
    if (texture.hasError()) {
      return std::move(texture).error();
    }
    resources.dummyClipMaskTexture = std::move(texture).result();

    const uint8_t mpixel[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    gpu::Status writeStatus =
        gpuDevice.writeTexture(resources.dummyClipMaskTexture, std::span<const uint8_t>(mpixel, 4),
                               gpu::TexelCopyBufferLayout{0, 256, 1}, gpu::Extent2d{1, 1});
    if (writeStatus.hasError()) {
      return std::move(writeStatus).error();
    }

    gpu::Result<gpu::TextureView> view = gpuDevice.createTextureView(
        resources.dummyClipMaskTexture, gpu::TextureViewDescriptor{"GeodeDeviceDummyClipMaskView"});
    if (view.hasError()) {
      return std::move(view).error();
    }
    resources.dummyClipMaskTextureView = std::move(view).result();

    gpu::Result<gpu::Sampler> sampler = gpuDevice.createSampler(gpu::SamplerDescriptor{
        "GeodeDeviceDummyClipMaskSampler", gpu::FilterMode::Linear, gpu::FilterMode::Linear,
        gpu::AddressMode::ClampToEdge, gpu::AddressMode::ClampToEdge});
    if (sampler.hasError()) {
      return std::move(sampler).error();
    }
    resources.dummyClipMaskSampler = std::move(sampler).result();
  }

  {
    // M6 Bullet 2: 1-element instance-transform buffer bound by every non-instanced solid
    // fill. Uploaded once, never modified. Layout matches the WGSL `InstanceTransform`
    // struct: two vec4f rows carrying the identity affine `{(1,0,0,0), (0,1,0,0)}`.
    const float identity[8] = {
        1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
    };
    gpu::Result<gpu::Buffer> buffer = gpuDevice.createBuffer(
        gpu::BufferDescriptor{"GeodeDeviceIdentityInstanceTransform", sizeof(identity),
                              gpu::BufferUsage::Storage | gpu::BufferUsage::CopyDst});
    if (buffer.hasError()) {
      return std::move(buffer).error();
    }
    resources.identityInstanceTransformBuffer = std::move(buffer).result();

    gpu::Status writeStatus = gpuDevice.writeBuffer(
        resources.identityInstanceTransformBuffer, 0,
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(identity), sizeof(identity)));
    if (writeStatus.hasError()) {
      return std::move(writeStatus).error();
    }
  }

  return resources;
}

void GeodeDevice::initSharedResources() {
  // The design-0053 adapter is created FIRST: the shared dummies below and the pipelines in
  // `initSharedPipelines` are all donner::gpu resources created through it.
  impl_->adapterDevice = std::make_unique<GeodeWgpuAdapterDevice>(*this);

  // Created before any setCounters() call so the allocations never count against per-frame
  // ceilings. Aborts on failure: the shared dummies are fixed-shape 1x1 resources against a
  // device that just initialized, so a creation error here is a lost device or a build defect,
  // not a recoverable runtime state.
  gpu::Result<GeodeSharedGpuResources> resources =
      GeodeSharedGpuResources::Create(*impl_->adapterDevice);
  if (resources.hasError()) {
    std::fprintf(stderr, "[Geode] shared GPU resource creation failed: %s\n",
                 resources.error().message.c_str());
    UTILS_RELEASE_ASSERT_MSG(false, "Geode shared-resource construction failed");
  }
  impl_->sharedResources = std::move(resources).result();
}

void GeodeDevice::initSharedPipelines() {
  // Requires device_ / queue_ / textureFormat_ to be fully populated and `initSharedResources`
  // (which creates the adapter + shared dummies) to have run. `CreateHeadless` and
  // `CreateFromExternal` both call this as the final step.
  const gpu::TextureFormat fmt = GpuTextureFormatFromWgpu(textureFormat_);

  impl_->pipeline = std::make_unique<GeodePipeline>(*impl_->adapterDevice, fmt);
  impl_->gradientPipeline = std::make_unique<GeodeGradientPipeline>(*impl_->adapterDevice, fmt);
  impl_->imagePipeline = std::make_unique<GeodeImagePipeline>(*impl_->adapterDevice, fmt);
  // Mask pipeline is built on first `maskPipeline()` access - see header.
  impl_->filterEngine = std::make_unique<GeodeFilterEngine>(*this, /*verbose=*/false);

  // Wire the donner::gpu recording context Geode's encoders draw through.
  impl_->gpuContext.gpuDevice = impl_->adapterDevice.get();
  impl_->gpuContext.geodeDevice = this;
  impl_->gpuContext.dummyPatternTextureView = &impl_->sharedResources.dummyPatternTextureView;
  impl_->gpuContext.dummyPatternSampler = &impl_->sharedResources.dummyPatternSampler;
  impl_->gpuContext.dummyClipMaskTextureView = &impl_->sharedResources.dummyClipMaskTextureView;
  impl_->gpuContext.dummyClipMaskSampler = &impl_->sharedResources.dummyClipMaskSampler;
  impl_->gpuContext.identityInstanceTransformBuffer =
      &impl_->sharedResources.identityInstanceTransformBuffer;
  impl_->gpuContext.residentBytesGauge = residentBytesGauge();
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

// ============================================================================
// GeodeGpuContext counter forwarding (declared in GeodeGpuContext.h; defined here where
// GeodeDevice is complete). Exact forwarders so production counters are unchanged; no-ops when
// the context is wired without a GeodeDevice (GPU-less test harnesses).
// ============================================================================

void GeodeGpuContext::countBuffer() const {
  if (geodeDevice != nullptr) {
    geodeDevice->countBuffer();
  }
}
void GeodeGpuContext::countTexture() const {
  if (geodeDevice != nullptr) {
    geodeDevice->countTexture();
  }
}
void GeodeGpuContext::countBindGroup() const {
  if (geodeDevice != nullptr) {
    geodeDevice->countBindGroup();
  }
}
void GeodeGpuContext::countDraw() const {
  if (geodeDevice != nullptr) {
    geodeDevice->countDraw();
  }
}
void GeodeGpuContext::countPipelineSwitch() const {
  if (geodeDevice != nullptr) {
    geodeDevice->countPipelineSwitch();
  }
}
void GeodeGpuContext::countPathEncode() const {
  if (geodeDevice != nullptr) {
    geodeDevice->countPathEncode();
  }
}
void GeodeGpuContext::countBufferWrite(uint64_t bytes) const {
  if (geodeDevice != nullptr) {
    geodeDevice->countBufferWrite(bytes);
  }
}
void GeodeGpuContext::countTextureWrite(uint64_t bytes) const {
  if (geodeDevice != nullptr) {
    geodeDevice->countTextureWrite(bytes);
  }
}
void GeodeGpuContext::countSubmit() const {
  if (geodeDevice != nullptr) {
    geodeDevice->countSubmit();
  }
}

GeodeMaskPipeline& GeodeGpuContext::maskPipeline() const {
  if (maskPipelineOverride != nullptr) {
    return *maskPipelineOverride;
  }
  return geodeDevice->maskPipeline();
}

}  // namespace donner::geode
