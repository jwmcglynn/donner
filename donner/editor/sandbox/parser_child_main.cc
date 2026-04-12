/// @file
///
/// Sandbox child binary: reads SVG bytes from stdin, parses, drives the
/// renderer via `SerializingRenderer`, and streams wire-format bytes to
/// stdout. Exits non-zero on any failure.
///
/// S3 protocol (one-shot):
/// - `argv`:   `donner_parser_child <width> <height>`
/// - `stdin`:  raw SVG bytes (read to EOF)
/// - `stdout`: wire-format message stream (see `donner/editor/sandbox/Wire.h`)
/// - `stderr`: human-readable diagnostics on any failure path
/// - exit:     see `SandboxProtocol.h` for exit-code classification
///
/// The child deliberately does **not** link a rasterizer. Pixel generation is
/// a host concern — the sandbox just converts an SVG bytestring into an
/// equivalent stream of `RendererInterface` calls that any host backend can
/// replay.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/editor/sandbox/SandboxHardening.h"
#include "donner/editor/sandbox/SandboxProtocol.h"
#include "donner/editor/sandbox/SerializingRenderer.h"
#include "donner/svg/SVG.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace {

constexpr int kMaxDimension = 16384;

std::string ReadAllStdin() {
  std::ostringstream oss;
  oss << std::cin.rdbuf();
  return std::move(oss).str();
}

}  // namespace

int main(int argc, char* argv[]) {
  using namespace donner;                  // NOLINT(google-build-using-namespace)
  using namespace donner::editor::sandbox;  // NOLINT(google-build-using-namespace)
  using namespace donner::svg;              // NOLINT(google-build-using-namespace)
  using namespace donner::svg::parser;      // NOLINT(google-build-using-namespace)

  if (argc != 3) {
    std::fprintf(stderr, "usage: donner_parser_child <width> <height>\n");
    return kExitUsageError;
  }

  const int width = std::atoi(argv[1]);
  const int height = std::atoi(argv[2]);
  if (width <= 0 || height <= 0 || width > kMaxDimension || height > kMaxDimension) {
    std::fprintf(stderr, "invalid dimensions: %dx%d\n", width, height);
    return kExitUsageError;
  }

  // Apply the S6 hardening profile BEFORE touching any untrusted input. This
  // covers RLIMIT caps, FD sweep, chdir(/), and the DONNER_SANDBOX env gate.
  // A non-kOk status here is fatal — the sandbox contract is all-or-nothing.
  const auto hardening = ApplyHardening();
  if (hardening.status != HardeningStatus::kOk) {
    std::fprintf(stderr, "sandbox hardening failed: %s\n", hardening.message.c_str());
    return kExitUsageError;
  }

  const std::string svg = ReadAllStdin();

  ParseWarningSink warnings;
  ParseResult<SVGDocument> maybeDocument = SVGParser::ParseSVG(svg, warnings);
  if (maybeDocument.hasError()) {
    std::ostringstream errStream;
    errStream << maybeDocument.error();
    std::fprintf(stderr, "parse error: %s\n", errStream.str().c_str());
    return kExitParseError;
  }

  SVGDocument document = std::move(maybeDocument.result());
  document.setCanvasSize(width, height);

  SerializingRenderer renderer;
  renderer.draw(document);

  const auto wire = renderer.data();
  if (wire.empty()) {
    std::fprintf(stderr, "empty wire stream from SerializingRenderer\n");
    return kExitRenderError;
  }

  if (std::fwrite(wire.data(), 1, wire.size(), stdout) != wire.size()) {
    std::fprintf(stderr, "stdout write truncated\n");
    return kExitRenderError;
  }
  std::fflush(stdout);

  if (renderer.hasUnsupported()) {
    // Non-fatal: the host will see `kEncounteredUnsupported` from the replayer
    // and surface it to the user. We still exit 0 because the frame was
    // emitted successfully — callers can inspect the stderr channel for the
    // specific count.
    std::fprintf(stderr, "warning: %zu unsupported call(s) encoded as kUnsupported\n",
                 renderer.unsupportedCount());
  }
  return kExitOk;
}
