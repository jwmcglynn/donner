#include "donner/gpu/baseline/WgpuBaselineCapture.h"

#include <atomic>
#include <cctype>
#include <fstream>
#include <string_view>
#include <utility>

#include "donner/gpu/baseline/FrozenBaselinePolicy.h"
#include "donner/svg/renderer/RendererImageIO.h"
#include "donner/svg/renderer/geode/GeoEncoder.h"
#include "donner/svg/renderer/geode/GeodeCallbackState.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeGpuWait.h"
#include "donner/svg/renderer/geode/GeodeImagePipeline.h"
#include "donner/svg/renderer/geode/GeodePipeline.h"
#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"

namespace donner::gpu::baseline {
namespace {

constexpr uint32_t kBytesPerRow = kCorpusSize * 4;  // 1024, already 256-byte aligned.
constexpr uint32_t kReadbackBytes = kBytesPerRow * kCorpusSize;

std::string_view BackendName(WGPUBackendType backend) {
  switch (backend) {
    case WGPUBackendType_Vulkan: return "Vulkan";
    case WGPUBackendType_Metal: return "Metal";
    case WGPUBackendType_D3D12: return "D3D12";
    case WGPUBackendType_D3D11: return "D3D11";
    case WGPUBackendType_OpenGL: return "OpenGL";
    case WGPUBackendType_OpenGLES: return "OpenGLES";
    case WGPUBackendType_WebGPU: return "WebGPU";
    case WGPUBackendType_Null: return "Null";
    default: return "Unknown";
  }
}

std::string_view AdapterTypeName(WGPUAdapterType type) {
  switch (type) {
    case WGPUAdapterType_DiscreteGPU: return "DiscreteGPU";
    case WGPUAdapterType_IntegratedGPU: return "IntegratedGPU";
    case WGPUAdapterType_CPU: return "CPU";
    default: return "Unknown";
  }
}

std::string ToString(const WGPUStringView& view) {
  return view.data != nullptr ? std::string(view.data, view.length) : std::string();
}

/// Joins the adapter's vendor and device strings into one name, skipping either when empty so a
/// driver that reports only one does not leave a stray separator in the frozen record.
std::string JoinAdapterName(const std::string& vendor, const std::string& device) {
  if (vendor.empty()) {
    return device;
  }
  if (device.empty()) {
    return vendor;
  }
  return vendor + " " + device;
}

CaptureEnvironment DescribeAdapter(const wgpu::Adapter& adapter) {
  CaptureEnvironment environment;
  environment.adapterName = "unknown";
  environment.adapterBackend = "Unknown";
  environment.adapterType = "Unknown";

  WGPUAdapterInfo info = {};
  if (wgpuAdapterGetInfo(adapter, &info) != WGPUStatus_Success) {
    return environment;
  }
  const std::string name = JoinAdapterName(ToString(info.vendor), ToString(info.device));
  if (!name.empty()) {
    environment.adapterName = name;
  }
  environment.adapterBackend = BackendName(info.backendType);
  environment.adapterType = AdapterTypeName(info.adapterType);
  wgpuAdapterInfoFreeMembers(info);
  return environment;
}

/// Names the render target through the runtime device so the encoder can bind it. Returns a
/// default-constructed handle on failure; the caller checks `hasResult()` before that happens.
gpu::Result<gpu::Texture> NameRenderTarget(geode::GeodeDevice& device,
                                           const wgpu::Texture& target) {
  return device.adapterDevice().importExternalTexture(
      target, gpu::Extent2d{kCorpusSize, kCorpusSize}, gpu::TextureFormat::RGBA8Unorm,
      gpu::TextureUsage::RenderAttachment | gpu::TextureUsage::CopySrc);
}

wgpu::Texture CreateRenderTarget(geode::GeodeDevice& device) {
  wgpu::TextureDescriptor descriptor = {};
  descriptor.label = geode::wgpuLabel("BaselineTarget");
  descriptor.size = {kCorpusSize, kCorpusSize, 1};
  descriptor.format = wgpu::TextureFormat::RGBA8Unorm;
  descriptor.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
  descriptor.mipLevelCount = 1;
  descriptor.sampleCount = 1;
  descriptor.dimension = wgpu::TextureDimension::_2D;
  return device.device().createTexture(descriptor);
}

wgpu::Buffer CreateReadbackBuffer(geode::GeodeDevice& device) {
  wgpu::BufferDescriptor descriptor = {};
  descriptor.label = geode::wgpuLabel("BaselineReadback");
  descriptor.size = kReadbackBytes;
  descriptor.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
  return device.device().createBuffer(descriptor);
}

void RecordScene(geode::GeodeDevice& device, const gpu::Texture& target, const CorpusScene& scene) {
  geode::GeoEncoder encoder(device, device.pipeline(), device.gradientPipeline(),
                            device.imagePipeline(), target,
                            gpu::Extent2d{kCorpusSize, kCorpusSize});
  encoder.clear(css::RGBA(0, 0, 0, 0));  // Transparent background.
  encoder.setTransform(tests::BaselinePixelFromScene());
  for (const tests::BaselinePathSpec& spec : scene.paths) {
    encoder.fillPath(spec.path, spec.color, spec.rule);
  }
  encoder.finish();
}

void RecordCopyToReadback(geode::GeodeDevice& device, const wgpu::Texture& target,
                          const wgpu::Buffer& readback) {
  wgpu::CommandEncoder encoder = device.device().createCommandEncoder();
  wgpu::TexelCopyTextureInfo source = {};
  source.texture = target;
  source.mipLevel = 0;
  source.origin = {0, 0, 0};
  wgpu::TexelCopyBufferInfo destination = {};
  destination.buffer = readback;
  destination.layout.bytesPerRow = kBytesPerRow;
  destination.layout.rowsPerImage = kCorpusSize;
  const wgpu::Extent3D copySize = {kCorpusSize, kCorpusSize, 1};
  encoder.copyTextureToBuffer(source, destination, copySize);
  wgpu::CommandBuffer commands = encoder.finish();
  device.queue().submit(1, &commands);
}

struct MapState {
  std::atomic<bool> done = false;
  std::atomic<bool> ok = false;
};

void OnBufferMapped(WGPUMapAsyncStatus status, WGPUStringView /*message*/, void* userdata1,
                    void* /*userdata2*/) {
  const std::shared_ptr<MapState> state = geode::takeWgpuCallbackState<MapState>(userdata1);
  state->ok.store(status == WGPUMapAsyncStatus_Success, std::memory_order_relaxed);
  state->done.store(true, std::memory_order_release);
}

/// Maps the readback buffer and copies its bytes out. Returns an empty string on success.
std::string ReadPixels(geode::GeodeDevice& device, const wgpu::Buffer& readback,
                       std::vector<uint8_t>& pixelsOut) {
  auto state = std::make_shared<MapState>();
  wgpu::BufferMapCallbackInfo callbackInfo{wgpu::Default};
  callbackInfo.callback = &OnBufferMapped;
  callbackInfo.userdata1 = geode::retainWgpuCallbackState(state);
  callbackInfo.userdata2 = nullptr;
  readback.mapAsync(wgpu::MapMode::Read, 0, kReadbackBytes, callbackInfo);

  const geode::GpuWaitResult waitResult = geode::BoundedGpuWait(
      [&] {
        device.device().poll(false, nullptr);
        return state->done.load(std::memory_order_acquire);
      },
      geode::kDefaultGpuWaitTimeout);
  if (waitResult != geode::GpuWaitResult::Complete) {
    return "readback buffer map wait timed out";
  }
  if (!state->ok.load(std::memory_order_relaxed)) {
    return "readback buffer map failed";
  }

  const auto* mapped = static_cast<const uint8_t*>(readback.getConstMappedRange(0, kReadbackBytes));
  if (mapped == nullptr) {
    return "readback buffer produced no mapped range";
  }
  pixelsOut.assign(mapped, mapped + kReadbackBytes);
  readback.unmap();
  return {};
}

/// Identifies the renderer the frozen bytes came from. The freeze is only an oracle for a
/// replacement backend if it is unambiguous which implementation produced it.
constexpr const char* kRendererPath = "wgpu-native Geode production path (GeodeDevice+GeoEncoder)";
constexpr const char* kRendererBackend = "geode";
constexpr const char* kTargetFormat = "RGBA8Unorm premultiplied, transparent background";

bool WriteProvenance(const std::filesystem::path& outputDir, const CaptureEnvironment& environment,
                     std::string_view sourceRevision, std::string_view sourceTree,
                     const std::string& capturedScenes) {
  std::ofstream out(outputDir / "capture_provenance.txt", std::ios::binary | std::ios::trunc);
  if (!out.good()) {
    return false;
  }
  out << "# Written by //donner/gpu/baseline:capture_baselines. Do not edit by hand.\n";
  out << "# Frozen pixels are only comparable against the adapter recorded here.\n";
  out << "schemaVersion: 1\n";
  out << "sourceRevision: " << sourceRevision << "\n";
  out << "sourceTreeClean: " << sourceTree << "\n";
  out << "rendererPath: " << kRendererPath << "\n";
  out << "rendererBackend: " << kRendererBackend << "\n";
  out << "adapterName: " << environment.adapterName << "\n";
  out << "adapterBackend: " << environment.adapterBackend << "\n";
  out << "adapterType: " << environment.adapterType << "\n";
  out << "targetFormat: " << kTargetFormat << "\n";
  out << "targetSize: " << kCorpusSize << "x" << kCorpusSize << "\n";
  out << "capturedScenes: " << capturedScenes << "\n";
  return out.good();
}

/// Renders and writes one scene. @return An empty string on success, or the failure reason.
std::string CaptureOneScene(WgpuBaselineCapturer& capturer, const CorpusScene& scene,
                            const std::filesystem::path& outputDir) {
  std::vector<uint8_t> pixels;
  const std::string error = capturer.capture(scene, pixels);
  if (!error.empty()) {
    return error;
  }
  const std::string path = (outputDir / (std::string(scene.name) + ".png")).string();
  if (!svg::RendererImageIO::writeRgbaPixelsToPngFile(path.c_str(), pixels, kCorpusSize,
                                                      kCorpusSize, kCorpusSize)) {
    return "failed to write " + path;
  }
  return {};
}

}  // namespace

std::string EnvironmentSlug(const CaptureEnvironment& environment) {
  return AdapterSlug(environment.adapterName, environment.adapterBackend);
}

WgpuBaselineCapturer::WgpuBaselineCapturer(std::unique_ptr<geode::GeodeDevice> device)
    : device_(std::move(device)), environment_(DescribeAdapter(device_->adapter())) {}

WgpuBaselineCapturer::~WgpuBaselineCapturer() = default;

std::unique_ptr<WgpuBaselineCapturer> WgpuBaselineCapturer::Create() {
  std::unique_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
  if (!device) {
    return nullptr;
  }
  return std::unique_ptr<WgpuBaselineCapturer>(new WgpuBaselineCapturer(std::move(device)));
}

std::string WgpuBaselineCapturer::capture(const CorpusScene& scene,
                                          std::vector<uint8_t>& pixelsOut) {
  const wgpu::Texture target = CreateRenderTarget(*device_);
  gpu::Result<gpu::Texture> targetHandle = NameRenderTarget(*device_, target);
  if (!targetHandle.hasResult()) {
    return "failed to name the baseline render target";
  }
  const wgpu::Buffer readback = CreateReadbackBuffer(*device_);

  RecordScene(*device_, std::move(targetHandle).result(), scene);
  RecordCopyToReadback(*device_, target, readback);
  return ReadPixels(*device_, readback, pixelsOut);
}

std::string WriteFrozenBaselineSet(WgpuBaselineCapturer& capturer,
                                   const std::filesystem::path& baselinesRoot,
                                   std::string_view sourceRevision, std::string_view sourceTree,
                                   std::filesystem::path* outputDirOut) {
  // One directory per adapter: a frozen capture is only comparable against the environment that
  // produced it, so environments accumulate side by side instead of overwriting each other.
  const std::filesystem::path outputDir = baselinesRoot / EnvironmentSlug(capturer.environment());
  if (outputDirOut != nullptr) {
    *outputDirOut = outputDir;
  }
  std::error_code directoryError;
  std::filesystem::create_directories(outputDir, directoryError);
  if (directoryError) {
    return "failed to create " + outputDir.string() + ": " + directoryError.message();
  }

  std::string capturedScenes;
  for (const CorpusScene& scene : Corpus()) {
    if (!scene.capturesPixels) {
      continue;
    }
    const std::string error = CaptureOneScene(capturer, scene, outputDir);
    if (!error.empty()) {
      return std::string(scene.name) + ": " + error;
    }
    if (!capturedScenes.empty()) {
      capturedScenes += ",";
    }
    capturedScenes += std::string(scene.name);
  }

  if (!WriteProvenance(outputDir, capturer.environment(), sourceRevision, sourceTree,
                       capturedScenes)) {
    return "failed to write the capture provenance record";
  }
  return {};
}

std::string CaptureNamedScene(WgpuBaselineCapturer& capturer, std::string_view sceneName,
                              std::vector<uint8_t>& pixelsOut) {
  for (const CorpusScene& scene : Corpus()) {
    if (scene.name == sceneName) {
      return capturer.capture(scene, pixelsOut);
    }
  }
  return "the corpus defines no scene named " + std::string(sceneName);
}

}  // namespace donner::gpu::baseline
