/// @file
///
/// `sandbox_replay` — loads a `.rnr` recording and rasterizes it via
/// `RendererTinySkia` to a PNG. Purely host-side: no sandbox child, no
/// parser, no network. This is the end-to-end proof that `.rnr` files are
/// self-contained and deterministic — the only inputs are the bytes on
/// disk.
///
/// Usage:
///     sandbox_replay <input.rnr> <output.png>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "donner/editor/sandbox/FrameInspector.h"
#include "donner/editor/sandbox/ReplayingRenderer.h"
#include "donner/editor/sandbox/RnrFile.h"
#include "donner/svg/renderer/RendererImageIO.h"
#include "donner/svg/renderer/RendererTinySkia.h"

namespace {

int Usage() {
  std::fprintf(stderr, "usage: sandbox_replay <input.rnr> <output.png>\n");
  return 64;
}

bool WritePngFile(const std::filesystem::path& path,
                  const std::vector<uint8_t>& bytes) {
  std::ofstream out(path, std::ios::binary);
  if (!out) return false;
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  return out.good();
}

const char* RnrStatusString(donner::editor::sandbox::RnrIoStatus status) {
  using donner::editor::sandbox::RnrIoStatus;
  switch (status) {
    case RnrIoStatus::kOk:               return "ok";
    case RnrIoStatus::kWriteFailed:      return "write failed";
    case RnrIoStatus::kReadFailed:       return "read failed";
    case RnrIoStatus::kTruncated:        return "truncated header";
    case RnrIoStatus::kMagicMismatch:    return "magic mismatch";
    case RnrIoStatus::kVersionMismatch:  return "version mismatch";
    case RnrIoStatus::kUriTooLong:       return "uri too long";
  }
  return "unknown";
}

}  // namespace

int main(int argc, char* argv[]) {
  using namespace donner::editor::sandbox;  // NOLINT(google-build-using-namespace)

  if (argc != 3) return Usage();

  const std::filesystem::path inputPath = argv[1];
  const std::filesystem::path outputPath = argv[2];

  RnrHeader header;
  std::vector<uint8_t> wire;
  const auto loadStatus = LoadRnrFile(inputPath, header, wire);
  if (loadStatus != RnrIoStatus::kOk) {
    std::fprintf(stderr, "sandbox_replay: load failed: %s\n", RnrStatusString(loadStatus));
    return 66;
  }

  std::fprintf(stdout, "sandbox_replay: loaded %s (%ux%u, backend hint=%u, uri='%s')\n",
               inputPath.string().c_str(), header.width, header.height,
               static_cast<uint32_t>(header.backend), header.uri.c_str());

  donner::svg::RendererTinySkia backend;
  const auto status = FrameInspector::ReplayPrefix(
      wire, std::numeric_limits<std::size_t>::max(), backend);
  if (status != ReplayStatus::kOk && status != ReplayStatus::kEncounteredUnsupported) {
    std::fprintf(stderr, "sandbox_replay: replay failed (status=%d)\n",
                 static_cast<int>(status));
    return 70;
  }
  if (status == ReplayStatus::kEncounteredUnsupported) {
    std::fprintf(stderr, "sandbox_replay: warning: stream contained unsupported messages\n");
  }

  const auto bitmap = backend.takeSnapshot();
  if (bitmap.dimensions.x <= 0 || bitmap.dimensions.y <= 0 || bitmap.pixels.empty()) {
    std::fprintf(stderr, "sandbox_replay: empty snapshot after replay\n");
    return 70;
  }
  const auto png = donner::svg::RendererImageIO::writeRgbaPixelsToPngMemory(
      bitmap.pixels, bitmap.dimensions.x, bitmap.dimensions.y,
      bitmap.rowBytes / 4);
  if (png.empty() || !WritePngFile(outputPath, png)) {
    std::fprintf(stderr, "sandbox_replay: failed to write PNG to %s\n",
                 outputPath.string().c_str());
    return 73;
  }
  std::fprintf(stdout, "sandbox_replay: wrote %s (%dx%d)\n", outputPath.string().c_str(),
               bitmap.dimensions.x, bitmap.dimensions.y);
  return 0;
}
