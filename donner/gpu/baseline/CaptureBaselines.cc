/// @file
/// Writes the frozen baseline set: one PNG per corpus scene plus the provenance record that says
/// exactly which revision, renderer path, and adapter produced them.
///
/// Run on a machine with a working GPU adapter; see baselines/README.md for the exact command.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "donner/gpu/baseline/WgpuBaselineCapture.h"
#include "donner/svg/renderer/RendererImageIO.h"

namespace donner::gpu::baseline {
namespace {

/// Identifies the renderer the frozen bytes came from. The freeze is only an oracle for a
/// replacement backend if it is unambiguous which implementation produced it.
constexpr const char* kRendererPath = "wgpu-native Geode production path (GeodeDevice+GeoEncoder)";
constexpr const char* kRendererBackend = "geode";
constexpr const char* kTargetFormat = "RGBA8Unorm premultiplied, transparent background";

std::string PngPathFor(const std::filesystem::path& outputDir, std::string_view sceneName) {
  return (outputDir / (std::string(sceneName) + ".png")).string();
}

bool WriteProvenance(const std::filesystem::path& outputDir, const CaptureEnvironment& environment,
                     const std::string& sourceRevision, const std::string& sourceTree,
                     const std::string& capturedScenes) {
  const std::filesystem::path path = outputDir / "capture_provenance.txt";
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
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
std::string CaptureOne(WgpuBaselineCapturer& capturer, const CorpusScene& scene,
                       const std::filesystem::path& outputDir) {
  std::vector<uint8_t> pixels;
  const std::string error = capturer.capture(scene, pixels);
  if (!error.empty()) {
    return error;
  }
  const std::string path = PngPathFor(outputDir, scene.name);
  if (!svg::RendererImageIO::writeRgbaPixelsToPngFile(path.c_str(), pixels, kCorpusSize,
                                                      kCorpusSize, kCorpusSize)) {
    return "failed to write " + path;
  }
  std::fprintf(stderr, "captured %s\n", path.c_str());
  return {};
}

int CaptureAll(const std::filesystem::path& outputDir, const std::string& sourceRevision,
               const std::string& sourceTree) {
  std::unique_ptr<WgpuBaselineCapturer> capturer = WgpuBaselineCapturer::Create();
  if (!capturer) {
    std::fprintf(stderr, "No GPU adapter available; the pixel freeze needs a real device\n");
    return 1;
  }

  std::string capturedScenes;
  for (const CorpusScene& scene : Corpus()) {
    if (!scene.capturesPixels) {
      continue;
    }
    const std::string error = CaptureOne(*capturer, scene, outputDir);
    if (!error.empty()) {
      std::fprintf(stderr, "%s: %s\n", std::string(scene.name).c_str(), error.c_str());
      return 1;
    }
    if (!capturedScenes.empty()) {
      capturedScenes += ",";
    }
    capturedScenes += std::string(scene.name);
  }

  if (!WriteProvenance(outputDir, capturer->environment(), sourceRevision, sourceTree,
                       capturedScenes)) {
    std::fprintf(stderr, "Failed to write the capture provenance record\n");
    return 1;
  }
  std::fprintf(stderr, "captured %s on %s (%s)\n", capturedScenes.c_str(),
               capturer->environment().adapterName.c_str(),
               capturer->environment().adapterBackend.c_str());
  return 0;
}

}  // namespace
}  // namespace donner::gpu::baseline

/// Entry point.
/// @param argc Argument count; three arguments are required.
/// @param argv `argv[1]` output directory, `argv[2]` source revision, `argv[3]` `clean`/`dirty`.
/// @return 0 when every scene was captured and the provenance record was written.
int main(int argc, char** argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: capture_baselines <output-dir> <source-revision> <clean|dirty>\n");
    return 2;
  }
  return donner::gpu::baseline::CaptureAll(argv[1], argv[2], argv[3]);
}
