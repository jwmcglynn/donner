#include <unistd.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "donner/editor/ipc/render_demo/render_messages.h"
#include "donner/editor/ipc/teleport/client.h"
#include "donner/editor/ipc/teleport/transport.h"

namespace {

namespace fs = std::filesystem;

std::string ResolveSiblingBinary(const char* argv0, const char* envName,
                                 const char* siblingName) {
  if (const char* override = std::getenv(envName); override && *override) {
    return override;
  }

  std::error_code ec;
  fs::path self(argv0);
  if (!self.has_parent_path()) {
    self = fs::absolute(self, ec);
  }

  const fs::path sibling = self.parent_path() / siblingName;
  if (fs::exists(sibling, ec)) {
    return sibling.string();
  }

  if (const char* runfilesDir = std::getenv("RUNFILES_DIR");
      runfilesDir && *runfilesDir) {
    const fs::path candidate = fs::path(runfilesDir) / "donner" / "donner" /
                               "editor" / "ipc" / "render_demo" / siblingName;
    if (fs::exists(candidate, ec)) {
      return candidate.string();
    }
  }

  return siblingName;
}

std::string ResolveDonnerSvg(const char* argv0) {
  if (const char* override = std::getenv("DONNER_TELEPORT_RENDER_TOOL");
      override && *override) {
    return override;
  }

  std::error_code ec;
  fs::path self = fs::absolute(fs::path(argv0), ec);
  // Try sibling first (Path B host build).
  fs::path sibling = self.parent_path() / "donner-svg";
  if (fs::exists(sibling, ec)) return sibling.string();

  // Try Bazel runfiles.
  if (const char* runfilesDir = std::getenv("RUNFILES_DIR");
      runfilesDir && *runfilesDir) {
    const fs::path candidate =
        fs::path(runfilesDir) / "donner" / "donner" / "svg" / "tool" /
        "donner-svg";
    if (fs::exists(candidate, ec)) return candidate.string();
  }

  return "donner-svg";
}

bool ReadAllFile(const fs::path& p, std::string& out) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

bool WriteAllFile(const fs::path& p, const std::vector<std::byte>& bytes) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  if (!bytes.empty()) {
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  }
  return out.good();
}

constexpr std::array<std::byte, 8> kPngSignature = {
    std::byte{0x89}, std::byte{0x50}, std::byte{0x4E}, std::byte{0x47},
    std::byte{0x0D}, std::byte{0x0A}, std::byte{0x1A}, std::byte{0x0A},
};

bool HasPngSignature(const std::vector<std::byte>& bytes) {
  if (bytes.size() < kPngSignature.size()) return false;
  return std::memcmp(bytes.data(), kPngSignature.data(),
                     kPngSignature.size()) == 0;
}

[[noreturn]] void Fail(const std::string& reason) {
  std::fprintf(stderr, "Teleport M2 proof of life: FAIL (%s)\n",
               reason.c_str());
  std::exit(1);
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc != 3) {
    std::fprintf(stderr, "Usage: %s <input.svg> <output.png>\n",
                 argc > 0 ? argv[0] : "render_demo");
    return 1;
  }

  const char* argv0 = argv[0];
  const fs::path inputSvgPath = argv[1];
  const fs::path outputPngPath = argv[2];

  std::string svgSource;
  if (!ReadAllFile(inputSvgPath, svgSource)) {
    Fail("failed to read input SVG: " + inputSvgPath.string());
  }
  if (svgSource.empty()) {
    Fail("input SVG is empty: " + inputSvgPath.string());
  }

  // Resolve donner-svg path and export to env so render_service can find it.
  const std::string donnerSvg = ResolveDonnerSvg(argv0);
  ::setenv("DONNER_TELEPORT_RENDER_TOOL", donnerSvg.c_str(), 1);

  const std::string serviceBin =
      ResolveSiblingBinary(argv0, "DONNER_TELEPORT_RENDER_SERVICE",
                           "render_service");

  donner::teleport::TeleportClient client({serviceBin});
  if (!client.isReady()) {
    const std::string reason = client.statusMessage().empty()
                                   ? "failed to spawn render_service"
                                   : std::string(client.statusMessage());
    Fail(reason);
  }

  donner::teleport::render_demo::RenderRequest req{
      .svg_source = std::move(svgSource), .width = 800, .height = 600};

  const auto start = std::chrono::steady_clock::now();
  auto resp = client.call<donner::teleport::render_demo::RenderRequest,
                          donner::teleport::render_demo::RenderResponse>(req);
  const auto end = std::chrono::steady_clock::now();

  if (!resp) {
    const std::string detail = client.statusMessage().empty()
                                   ? std::string("transport error")
                                   : std::string(client.statusMessage());
    Fail("Teleport call failed: " + detail);
  }

  if (resp->png_bytes.empty()) {
    Fail("render_service returned empty PNG (see stderr from service)");
  }

  if (!HasPngSignature(resp->png_bytes)) {
    Fail("response does not start with PNG signature");
  }

  if (!WriteAllFile(outputPngPath, resp->png_bytes)) {
    Fail("failed to write output PNG: " + outputPngPath.string());
  }

  const auto roundTripMs =
      std::chrono::duration_cast<std::chrono::microseconds>(end - start)
          .count() /
      1000.0;

  std::printf("Teleport M2 proof of life: PASS\n");
  std::printf(
      "  Wrote %s (%zu bytes, first 8 bytes valid PNG signature)\n",
      outputPngPath.c_str(), resp->png_bytes.size());
  std::printf(
      "  Round-trip (parse + render + transport + decode): %.2f ms\n",
      roundTripMs);
  return 0;
}
