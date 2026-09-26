#include "donner/gpu/baseline/WgpuBaselineCapture.h"

#include <fstream>
#include <string_view>
#include <utility>

#include "donner/gpu/baseline/FrozenBaselinePolicy.h"
#include "donner/svg/renderer/RendererImageIO.h"
#include "donner/svg/renderer/geode/GeoEncoder.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeImagePipeline.h"
#include "donner/svg/renderer/geode/GeodeNativeRoot.h"
#include "donner/svg/renderer/geode/GeodePipeline.h"
#include "donner/svg/renderer/geode/tests/GeodeTestContexts.h"
#if defined(__APPLE__)
#include "donner/gpu/metal/MetalDevice.h"
#endif
#if defined(__linux__)
#include "donner/gpu/vulkan/VulkanDevice.h"
#endif

namespace donner::gpu::baseline {
namespace {

CaptureEnvironment DescribeAdapter(const geode::GeodeDevice& device) {
  CaptureEnvironment environment;
  environment.adapterType = "Unknown";
#if defined(__APPLE__)
  environment.adapterBackend = "Metal";
  environment.adapterName =
      static_cast<const gpu::metal::MetalDevice&>(device.runtimeDevice()).adapterName();
#elif defined(__linux__)
  environment.adapterBackend = "Vulkan";
  const auto& nativeRoot = device.physicalDeviceOwner()->root().vulkanRoot();
  if (nativeRoot != nullptr) {
    environment.adapterName = nativeRoot->adapterName();
  }
#endif
  return environment;
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

/// Identifies the renderer the frozen bytes came from. The freeze is only an oracle for a
/// replacement backend if it is unambiguous which implementation produced it.
constexpr const char* kRendererPath = "native Geode production path (GeodeDevice+GeoEncoder)";
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
    : device_(std::move(device)), environment_(DescribeAdapter(*device_)) {}

WgpuBaselineCapturer::~WgpuBaselineCapturer() = default;

std::unique_ptr<WgpuBaselineCapturer> WgpuBaselineCapturer::Create() {
  geode::GpuRootSelection selection;
  selection.label = "FrozenBaselineCapture";
#if defined(__APPLE__)
  selection.backend = geode::GpuBackendKind::NativeMetal;
#elif defined(__linux__)
  selection.backend = geode::GpuBackendKind::NativeVulkan;
#else
  return nullptr;
#endif
  std::unique_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateOverSelectedRoot(
      geode::SelectGpuRoot(selection), gpu::TextureFormat::RGBA8Unorm);
  if (!device) {
    return nullptr;
  }
  auto capturer =
      std::unique_ptr<WgpuBaselineCapturer>(new WgpuBaselineCapturer(std::move(device)));
  return capturer->environment().adapterName.empty() ? nullptr : std::move(capturer);
}

std::string WgpuBaselineCapturer::capture(const CorpusScene& scene,
                                          std::vector<uint8_t>& pixelsOut) {
  gpu::Result<gpu::Texture> target = device_->runtimeDevice().createTexture(gpu::TextureDescriptor{
      "BaselineTarget", gpu::Extent2d{kCorpusSize, kCorpusSize}, gpu::TextureFormat::RGBA8Unorm,
      gpu::TextureUsage::RenderAttachment | gpu::TextureUsage::CopySrc});
  if (target.hasError()) {
    return "failed to create the baseline render target: " + target.error().message;
  }
  RecordScene(*device_, target.result(), scene);
  gpu::Result<std::vector<uint8_t>> pixels = geode::ReadTexturePixels(
      device_->runtimeDevice(), target.result(), gpu::Extent2d{kCorpusSize, kCorpusSize});
  if (pixels.hasError()) {
    return "failed to read the baseline pixels: " + pixels.error().message;
  }
  pixelsOut = std::move(pixels).result();
  return {};
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
