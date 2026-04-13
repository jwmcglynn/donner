/// @file
///
/// `sandbox_render` — command-line tool that exercises the full editor
/// sandbox pipeline end-to-end:
///
///     SvgSource -> SandboxHost -> donner_parser_child -> wire format
///                -> ReplayingRenderer -> RendererTinySkia -> PNG
///
/// It's the address bar's code path with the UI replaced by argv. When the
/// editor's M2 actually lands, `EditorApp::navigate(uri)` will call the same
/// sequence of functions — this CLI is how we prove the sequence works
/// before there's a UI to drive it.
///
/// Usage:
///     sandbox_render URI WIDTH HEIGHT OUTPUT_PNG
///
/// The child binary path is expected to sit next to this binary in the same
/// runfiles tree (`donner/editor/sandbox/donner_parser_child`). Override
/// with `DONNER_PARSER_CHILD=/abs/path` if you're running outside Bazel's
/// runfiles layout.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "donner/editor/sandbox/SandboxHost.h"
#include "donner/editor/sandbox/SvgSource.h"
#include "donner/svg/renderer/RendererImageIO.h"
#include "donner/svg/renderer/RendererTinySkia.h"

namespace {

int Usage() {
  std::fprintf(stderr, "usage: sandbox_render <uri> <width> <height> <output.png>\n");
  return 64;  // EX_USAGE
}

std::string ResolveChildPath(const char* argv0) {
  if (const char* override = std::getenv("DONNER_PARSER_CHILD"); override && *override) {
    return override;
  }
  // Fall back to a sibling binary in the same runfiles directory as argv[0].
  // Works under `bazel run` and for installed builds alike.
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path self(argv0);
  const fs::path parent = self.parent_path();
  const fs::path sibling = parent / "donner_parser_child";
  if (fs::exists(sibling, ec)) {
    return sibling.string();
  }
  // Last resort: let posix_spawn search PATH.
  return "donner_parser_child";
}

bool WritePngFile(const std::filesystem::path& path,
                  const std::vector<uint8_t>& bytes) {
  std::ofstream out(path, std::ios::binary);
  if (!out) return false;
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  return out.good();
}

}  // namespace

int main(int argc, char* argv[]) {
  using namespace donner::editor::sandbox;  // NOLINT(google-build-using-namespace)

  if (argc != 5) return Usage();

  const char* uri = argv[1];
  const int width = std::atoi(argv[2]);
  const int height = std::atoi(argv[3]);
  const std::filesystem::path outputPath = argv[4];

  if (width <= 0 || height <= 0) {
    std::fprintf(stderr, "sandbox_render: width and height must be positive integers\n");
    return Usage();
  }

  // Step 1: fetch the SVG bytes (host-side privilege).
  SvgSource source;
  const auto fetch = source.fetch(uri);
  if (fetch.status != SvgFetchStatus::kOk) {
    std::fprintf(stderr, "sandbox_render: fetch failed: %s\n", fetch.diagnostics.c_str());
    return 66;  // EX_NOINPUT
  }

  // Step 2: hand the bytes to the sandbox child, replay its wire stream into
  // a local RendererTinySkia backend.
  SandboxHost host(ResolveChildPath(argv[0]));
  const std::string_view svgView(reinterpret_cast<const char*>(fetch.bytes.data()),
                                 fetch.bytes.size());

  donner::svg::RendererTinySkia backend;
  const auto result = host.renderToBackend(svgView, width, height, backend);
  if (result.status != SandboxStatus::kOk) {
    std::fprintf(stderr, "sandbox_render: sandbox render failed (status=%d, exit=%d): %s\n",
                 static_cast<int>(result.status), result.exitCode, result.diagnostics.c_str());
    return 70;  // EX_SOFTWARE
  }
  if (result.unsupportedCount > 0) {
    std::fprintf(stderr, "sandbox_render: warning: %u unsupported call(s) skipped\n",
                 result.unsupportedCount);
  }

  // Step 3: encode the backend's snapshot to PNG and write it out.
  const donner::svg::RendererBitmap bitmap = backend.takeSnapshot();
  if (bitmap.dimensions.x <= 0 || bitmap.dimensions.y <= 0 || bitmap.pixels.empty()) {
    std::fprintf(stderr, "sandbox_render: empty snapshot after replay\n");
    return 70;
  }

  const auto png = donner::svg::RendererImageIO::writeRgbaPixelsToPngMemory(
      bitmap.pixels, bitmap.dimensions.x, bitmap.dimensions.y,
      bitmap.rowBytes / 4);
  if (png.empty() || !WritePngFile(outputPath, png)) {
    std::fprintf(stderr, "sandbox_render: failed to write PNG to %s\n",
                 outputPath.string().c_str());
    return 73;  // EX_CANTCREAT
  }

  std::printf("sandbox_render: wrote %s (%dx%d)\n", outputPath.string().c_str(),
              bitmap.dimensions.x, bitmap.dimensions.y);
  return 0;
}
