/// @file
///
/// `sandbox_inspect` — loads a `.rnr` recording and prints the decoded
/// command stream as an indented text table. It's the text-mode version of
/// the eventual ImGui frame inspector panel described in
/// `docs/design_docs/editor_sandbox.md` S4, and usable today for debugging
/// and bug reports even before the editor UI lands.
///
/// Usage:
///     sandbox_inspect <input.rnr>

#include <cstdio>
#include <filesystem>
#include <vector>

#include "donner/editor/sandbox/FrameInspector.h"
#include "donner/editor/sandbox/RnrFile.h"

int main(int argc, char* argv[]) {
  using namespace donner::editor::sandbox;  // NOLINT(google-build-using-namespace)

  if (argc != 2) {
    std::fprintf(stderr, "usage: sandbox_inspect <input.rnr>\n");
    return 64;
  }

  const std::filesystem::path inputPath = argv[1];
  RnrHeader header;
  std::vector<uint8_t> wire;
  const auto loadStatus = LoadRnrFile(inputPath, header, wire);
  if (loadStatus != RnrIoStatus::kOk) {
    std::fprintf(stderr, "sandbox_inspect: load failed (status=%d)\n",
                 static_cast<int>(loadStatus));
    return 66;
  }

  std::fprintf(stdout, "# rnr file: %s\n", inputPath.string().c_str());
  std::fprintf(stdout, "# viewport: %ux%u\n", header.width, header.height);
  std::fprintf(stdout, "# backend:  %u\n", static_cast<uint32_t>(header.backend));
  std::fprintf(stdout, "# uri:      %s\n", header.uri.c_str());
  std::fprintf(stdout, "# wire bytes: %zu\n", wire.size());
  std::fprintf(stdout, "\n");

  const std::string dump = FrameInspector::Dump(wire);
  std::fwrite(dump.data(), 1, dump.size(), stdout);
  return 0;
}
