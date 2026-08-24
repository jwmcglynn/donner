/// @file
/// Writes the frozen baseline set for the adapter this machine has: one PNG per corpus scene plus
/// the provenance record that says which revision, renderer path, and adapter produced them.
///
/// Run on a machine with a working GPU adapter; see README.md for the exact command.

#include <cstdio>
#include <filesystem>
#include <memory>

#include "donner/gpu/baseline/WgpuBaselineCapture.h"

/// Entry point.
/// @param argc Argument count; three arguments are required.
/// @param argv `argv[1]` baselines root, `argv[2]` source revision, `argv[3]` `clean`/`dirty`.
/// @return 0 when every scene was captured and the provenance record was written.
int main(int argc, char** argv) {
  using donner::gpu::baseline::WgpuBaselineCapturer;

  if (argc != 4) {
    std::fprintf(stderr,
                 "usage: capture_baselines <baselines-root> <source-revision> <clean|dirty>\n");
    return 2;
  }

  std::unique_ptr<WgpuBaselineCapturer> capturer = WgpuBaselineCapturer::Create();
  if (!capturer) {
    std::fprintf(stderr, "No GPU adapter available; the pixel freeze needs a real device\n");
    return 1;
  }

  std::filesystem::path outputDir;
  const std::string error = donner::gpu::baseline::WriteFrozenBaselineSet(
      *capturer, argv[1], argv[2], argv[3], &outputDir);
  if (!error.empty()) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  std::fprintf(stderr, "captured the corpus into %s\n", outputDir.string().c_str());
  return 0;
}
