// Exactly one translation unit in the binary must define
// `WEBGPU_CPP_IMPLEMENTATION` before including `<webgpu/webgpu.hpp>`. The
// header ships the body of every C++ wrapper method inside a
// `#ifdef WEBGPU_CPP_IMPLEMENTATION` block; without this define the
// wrapper methods are declared but never defined, and linking fails with
// unresolved `wgpu::Instance::requestAdapter` and friends.
#define WEBGPU_CPP_IMPLEMENTATION
#include "donner/svg/renderer/geode/GeodeDevice.h"

#include <cstdio>

#include "donner/svg/renderer/geode/GeodeFilterEngine.h"
#include "donner/svg/renderer/geode/GeodeImagePipeline.h"
#include "donner/svg/renderer/geode/GeodePipeline.h"

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

}  // namespace

/// PIMPL struct: holds the wgpu::Instance so its lifetime is tied to
/// the GeodeDevice wrapper. Adapter/device/queue handles are stored
/// directly on the outer class. In embedded mode, `instance` is null
/// because the host owns the instance.
struct GeodeDevice::Impl {
  wgpu::Instance instance;

  // Shared dummies used by every GeoEncoder's bind groups — see
  // comment block on `GeodeDevice::dummyPatternTexture()`. Created
  // once at `CreateHeadless` time so they never count against
  // per-frame `textureCreates` ceilings.
  wgpu::Texture dummyPatternTexture;
  wgpu::TextureView dummyPatternTextureView;
  wgpu::Sampler dummyPatternSampler;
  wgpu::Texture dummyClipMaskTexture;
  wgpu::TextureView dummyClipMaskTextureView;
  wgpu::Sampler dummyClipMaskSampler;

  // M6 Bullet 2: 1-element instance-transform buffer bound by every
  // non-instanced solid fill. Uploaded once at CreateHeadless time,
  // never modified. Layout matches the WGSL `InstanceTransform`
  // struct: two vec4f rows carrying the identity affine
  // `{(1,0,0,0), (0,1,0,0)}`.
  wgpu::Buffer identityInstanceTransformBuffer;

  // Shared render / compute pipelines. Constructed once per GeodeDevice
  // in `initSharedPipelines` — see the public `pipeline()` / … / `filterEngine()`
  // accessors on GeodeDevice for the "why" behind sharing. These fields
  // are at the bottom of Impl so they destruct before the wgpu::Device
  // at the top of GeodeDevice (reverse-declaration order).
  std::unique_ptr<GeodePipeline> pipeline;
  std::unique_ptr<GeodeGradientPipeline> gradientPipeline;
  std::unique_ptr<GeodeImagePipeline> imagePipeline;
  /// Built lazily on first `maskPipeline()` access — see the header.
  std::unique_ptr<GeodeMaskPipeline> maskPipeline;
  std::unique_ptr<GeodeFilterEngine> filterEngine;
};

GeodeDevice::GeodeDevice() : impl_(std::make_unique<Impl>()) {}
GeodeDevice::~GeodeDevice() = default;

std::unique_ptr<GeodeDevice> GeodeDevice::CreateHeadless() {
  auto result = std::unique_ptr<GeodeDevice>(new GeodeDevice());

  // 1. Create the WebGPU instance. wgpu-native's `wgpuCreateInstance`
  //    is synchronous and never blocks on I/O; the returned handle is
  //    the root of the object graph.
  result->impl_->instance = wgpu::createInstance();
  if (!result->impl_->instance) {
    std::fprintf(stderr, "[Geode/wgpu-native] wgpuCreateInstance returned null\n");
    return nullptr;
  }

  // 2. Request a GPU adapter. The synchronous form in webgpu-cpp
  //    internally calls the async `wgpuInstanceRequestAdapter` with a
  //    lambda that parks the result on the stack — wgpu-native invokes
  //    the callback before returning from the request, so the sync
  //    form is safe on native targets (Emscripten loops on
  //    `emscripten_sleep` instead).
  wgpu::RequestAdapterOptions adapterOptions = {};
  result->adapter_ = result->impl_->instance.requestAdapter(adapterOptions);
  if (!result->adapter_) {
    std::fprintf(stderr, "[Geode/wgpu-native] No WebGPU adapter available.\n");
    return nullptr;
  }

  // Log adapter selection so it is obvious at a glance whether we landed
  // on a discrete GPU / integrated GPU / software rasterizer, and which
  // native backend (Vulkan / Metal / D3D12 / …) is driving it.
  //
  // Under Emscripten the wgpuAdapterGetInfo / WGPUAdapterInfo struct
  // layout may differ from wgpu-native v24 (WGPUStringView fields vs raw
  // char*). Skip the native-specific logging for now — the browser's
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

      // Intel + Vulkan: writing @builtin(sample_mask) from overlapping band
      // quads hangs Mesa ANV / Xe KMD when bandCount >= 2 (observed on Arc
      // A380, Mesa 25.2.8). Fall back to alpha-coverage AA which folds
      // coverage into the fragment color and runs at sampleCount = 1 (no
      // MSAA texture, no hardware resolve) to avoid a second class of flaky
      // MSAA-resolve hangs on the same driver.
      if (info.vendorID == 0x8086 && info.backendType == WGPUBackendType_Vulkan) {
        result->useAlphaCoverageAA_ = true;
        std::fprintf(stderr,
                     "[Geode] Intel Arc + Vulkan detected; using alpha-coverage AA "
                     "(sample_mask output hangs on Mesa 25.2.8 Xe-KMD; "
                     "upstream fix in Mesa 25.3)\n");
      }

      wgpuAdapterInfoFreeMembers(info);
    }
  }
#else
  std::fprintf(stderr, "[Geode/emscripten] WebGPU adapter acquired (browser-managed).\n");
#endif

  // 3. Create the device. Error diagnostics are wired via
  //    `uncapturedErrorCallbackInfo` on the descriptor — the callback
  //    stays valid for the device's lifetime.
  //
  // DeviceLostCallback is intentionally NOT set. wgpu-native fires it
  // during normal device destruction and the callback path interacts
  // badly with static destruction order for globals.
  wgpu::DeviceDescriptor deviceDesc = {};
  deviceDesc.label = wgpu::StringView{std::string_view{"GeodeDevice"}};
  deviceDesc.uncapturedErrorCallbackInfo.callback = OnUncapturedError;
  deviceDesc.uncapturedErrorCallbackInfo.userdata1 = nullptr;
  deviceDesc.uncapturedErrorCallbackInfo.userdata2 = nullptr;

  result->device_ = result->adapter_.requestDevice(deviceDesc);
  if (!result->device_) {
    std::fprintf(stderr, "[Geode/wgpu-native] Failed to create device.\n");
    return nullptr;
  }

  // 4. Grab the default queue.
  result->queue_ = result->device_.getQueue();
  if (!result->queue_) {
    std::fprintf(stderr, "[Geode/wgpu-native] Failed to get queue.\n");
    return nullptr;
  }

  // 5. Create the shared dummy textures / samplers used by every
  // GeoEncoder. These are 1×1 "identity" fills for the pattern and
  // clip-mask bind slots when the current draw doesn't actually use
  // them. Previously each GeoEncoder allocated its own pair, which
  // showed up as 2+ `textureCreates` per frame for every
  // push/pop layer/filter/mask encoder. See design doc 0030 §M4.2.
  //
  // Created here (before any `setCounters()` call) so the allocations
  // never count against per-frame ceilings.
  {
    // Pattern dummy: 1×1 RGBA8 opaque black.
    wgpu::TextureDescriptor td = {};
    td.label = wgpu::StringView{std::string_view{"GeodeDeviceDummyPattern"}};
    td.size = {1u, 1u, 1u};
    td.format = wgpu::TextureFormat::RGBA8Unorm;
    td.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    td.mipLevelCount = 1;
    td.sampleCount = 1;
    td.dimension = wgpu::TextureDimension::_2D;
    result->impl_->dummyPatternTexture = result->device_.createTexture(td);

    const uint8_t pixel[4] = {0, 0, 0, 255};
    wgpu::TexelCopyTextureInfo dst = {};
    dst.texture = result->impl_->dummyPatternTexture;
    wgpu::TexelCopyBufferLayout layout = {};
    layout.bytesPerRow = 4;
    layout.rowsPerImage = 1;
    wgpu::Extent3D extent = {1u, 1u, 1u};
    result->queue_.writeTexture(dst, pixel, sizeof(pixel), layout, extent);
    result->impl_->dummyPatternTextureView = result->impl_->dummyPatternTexture.createView();

    wgpu::SamplerDescriptor sd{wgpu::Default};
    sd.label = wgpu::StringView{std::string_view{"GeodeDeviceDummyPatternSampler"}};
    sd.addressModeU = wgpu::AddressMode::Repeat;
    sd.addressModeV = wgpu::AddressMode::Repeat;
    sd.minFilter = wgpu::FilterMode::Linear;
    sd.magFilter = wgpu::FilterMode::Linear;
    sd.maxAnisotropy = 1;
    result->impl_->dummyPatternSampler = result->device_.createSampler(sd);
  }

  {
    // Clip-mask dummy: 1×1 RGBA8Unorm with all channels 0xFF (= 1.0 coverage per
    // sample lane in the alpha-coverage packed-mask path introduced by the
    // Phase 3b clip-mask fix).
    wgpu::TextureDescriptor md = {};
    md.label = wgpu::StringView{std::string_view{"GeodeDeviceDummyClipMask"}};
    md.size = {1u, 1u, 1u};
    md.format = wgpu::TextureFormat::RGBA8Unorm;
    md.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    md.mipLevelCount = 1;
    md.sampleCount = 1;
    md.dimension = wgpu::TextureDimension::_2D;
    result->impl_->dummyClipMaskTexture = result->device_.createTexture(md);

    const uint8_t mpixel[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    wgpu::TexelCopyTextureInfo mdst = {};
    mdst.texture = result->impl_->dummyClipMaskTexture;
    wgpu::TexelCopyBufferLayout mlayout = {};
    mlayout.bytesPerRow = 4;
    mlayout.rowsPerImage = 1;
    wgpu::Extent3D mextent = {1u, 1u, 1u};
    result->queue_.writeTexture(mdst, mpixel, sizeof(mpixel), mlayout, mextent);
    result->impl_->dummyClipMaskTextureView = result->impl_->dummyClipMaskTexture.createView();

    wgpu::SamplerDescriptor msd{wgpu::Default};
    msd.label = wgpu::StringView{std::string_view{"GeodeDeviceDummyClipMaskSampler"}};
    msd.addressModeU = wgpu::AddressMode::ClampToEdge;
    msd.addressModeV = wgpu::AddressMode::ClampToEdge;
    msd.minFilter = wgpu::FilterMode::Linear;
    msd.magFilter = wgpu::FilterMode::Linear;
    msd.maxAnisotropy = 1;
    result->impl_->dummyClipMaskSampler = result->device_.createSampler(msd);
  }

  // M6 Bullet 2: identity instance-transform buffer for non-instanced
  // solid fills. Two vec4f rows = 32 bytes, uploaded once.
  {
    const float identity[8] = {
        1.0f, 0.0f, 0.0f, 0.0f,  // row0 = (a, c, e, _pad) = identity X
        0.0f, 1.0f, 0.0f, 0.0f,  // row1 = (b, d, f, _pad) = identity Y
    };
    wgpu::BufferDescriptor bd = {};
    bd.label = wgpu::StringView{std::string_view{"GeodeDeviceIdentityInstanceTransform"}};
    bd.size = sizeof(identity);
    bd.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
    result->impl_->identityInstanceTransformBuffer = result->device_.createBuffer(bd);
    result->queue_.writeBuffer(result->impl_->identityInstanceTransformBuffer, 0, identity,
                               sizeof(identity));
  }

  result->initSharedPipelines();

  return result;
}

const wgpu::Texture& GeodeDevice::dummyPatternTexture() const {
  return impl_->dummyPatternTexture;
}
const wgpu::TextureView& GeodeDevice::dummyPatternTextureView() const {
  return impl_->dummyPatternTextureView;
}
const wgpu::Sampler& GeodeDevice::dummyPatternSampler() const {
  return impl_->dummyPatternSampler;
}
const wgpu::Texture& GeodeDevice::dummyClipMaskTexture() const {
  return impl_->dummyClipMaskTexture;
}
const wgpu::TextureView& GeodeDevice::dummyClipMaskTextureView() const {
  return impl_->dummyClipMaskTextureView;
}
const wgpu::Sampler& GeodeDevice::dummyClipMaskSampler() const {
  return impl_->dummyClipMaskSampler;
}
const wgpu::Buffer& GeodeDevice::identityInstanceTransformBuffer() const {
  return impl_->identityInstanceTransformBuffer;
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
    impl_->maskPipeline =
        std::make_unique<GeodeMaskPipeline>(device_, useAlphaCoverageAA_, sampleCount());
  }
  return *impl_->maskPipeline;
}
GeodeFilterEngine& GeodeDevice::filterEngine() const {
  return *impl_->filterEngine;
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
  // impl_->instance stays null — host owns the instance.

  // Use the host-provided adapter for hardware workaround detection. When no
  // adapter is supplied, skip detection — the host controls its own device.
  if (config.adapter) {
    result->adapter_ = config.adapter;
    WGPUAdapterInfo info = {};
    if (wgpuAdapterGetInfo(result->adapter_, &info) == WGPUStatus_Success) {
      if (info.vendorID == 0x8086 && info.backendType == WGPUBackendType_Vulkan) {
        result->useAlphaCoverageAA_ = true;
        std::fprintf(stderr,
                     "[Geode] Intel Arc + Vulkan detected (embedded); "
                     "using alpha-coverage AA\n");
      }
      wgpuAdapterInfoFreeMembers(info);
    }
  }

  result->supportsTimestamps_ = config.device.hasFeature(wgpu::FeatureName::TimestampQuery);

  result->initSharedPipelines();

  return result;
}

void GeodeDevice::initSharedPipelines() {
  // Requires device_ / queue_ / textureFormat_ / useAlphaCoverageAA_ to
  // be fully populated — `CreateHeadless` and `CreateFromExternal` both
  // call this as the final step.
  const wgpu::TextureFormat fmt = textureFormat_;
  const uint32_t samples = sampleCount();
  const bool alphaCoverage = useAlphaCoverageAA_;

  impl_->pipeline = std::make_unique<GeodePipeline>(device_, fmt, alphaCoverage, samples);
  impl_->gradientPipeline =
      std::make_unique<GeodeGradientPipeline>(device_, fmt, alphaCoverage, samples);
  impl_->imagePipeline = std::make_unique<GeodeImagePipeline>(device_, fmt, samples);
  // Mask pipeline is built on first `maskPipeline()` access — see header.
  impl_->filterEngine = std::make_unique<GeodeFilterEngine>(*this, /*verbose=*/false);
}

void GeodeDevice::deferDestroy(wgpu::Buffer buffer) {
  if (buffer) {
    pendingBuffers_.push_back(std::move(buffer));
  }
}

void GeodeDevice::deferDestroy(wgpu::Texture texture) {
  if (texture) {
    pendingTextures_.push_back(std::move(texture));
  }
}

void GeodeDevice::drainDeferredDestroys() {
  pendingBuffers_.clear();
  pendingTextures_.clear();
}

}  // namespace donner::geode
