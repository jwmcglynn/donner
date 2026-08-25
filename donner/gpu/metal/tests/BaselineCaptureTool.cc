/// @file
/// Writes the Metal slice's golden PNG by rendering the shared baseline scene through the current
/// production renderer as a black box.
///
/// The rendering itself lives in the frozen-baseline capture library, so this tool and the frozen
/// corpus cannot drift into two different setups of the same scene. Run on the target machine:
///   bazel run //donner/gpu/metal/tests:baseline_capture_tool -- \
///     $(bazel info workspace)/donner/gpu/metal/tests/testdata/solid_fill_baseline.png

#include <cstdio>
#include <memory>
#include <vector>

#include "donner/gpu/baseline/WgpuBaselineCapture.h"
#include "donner/svg/renderer/RendererImageIO.h"

/// Entry point. @param argc Argument count. @param argv `argv[1]` is the output PNG path.
/// @return 0 when the scene was captured and written.
int main(int argc, char** argv) {
  using donner::gpu::baseline::kCorpusSize;
  using donner::gpu::baseline::WgpuBaselineCapturer;

  if (argc != 2) {
    std::fprintf(stderr, "usage: baseline_capture_tool <output.png>\n");
    return 2;
  }

  std::unique_ptr<WgpuBaselineCapturer> capturer = WgpuBaselineCapturer::Create();
  if (!capturer) {
    std::fprintf(stderr, "No GPU adapter available for baseline capture\n");
    return 1;
  }

  std::vector<uint8_t> pixels;
  const std::string error =
      donner::gpu::baseline::CaptureNamedScene(*capturer, "solid_fill_baseline", pixels);
  if (!error.empty()) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }

  if (!donner::svg::RendererImageIO::writeRgbaPixelsToPngFile(argv[1], pixels, kCorpusSize,
                                                              kCorpusSize, kCorpusSize)) {
    std::fprintf(stderr, "Failed to write %s\n", argv[1]);
    return 1;
  }
  std::fprintf(stderr, "Baseline written to %s on %s (%s)\n", argv[1],
               capturer->environment().adapterName.c_str(),
               capturer->environment().adapterBackend.c_str());
  return 0;
}
