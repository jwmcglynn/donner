/// @file
/// Minimal executable for code-size measurement: parses a small embedded document and renders it
/// once through the first backend registered by the one backend library linked into the binary
/// (a library may register several configurations of the same code), printing a checksum of the
/// frame so no rendering code can be discarded as dead. Every code-size binary uses this source and
/// identical flags; only the linked backend library differs.

#include <cstdint>
#include <cstdio>
#include <memory>

#include "donner/base/ParseWarningSink.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/parser/SVGParser.h"
#include "harness/BenchBackend.h"

namespace {

constexpr const char kScene[] = R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64" viewBox="0 0 64 64">
  <linearGradient id="g"><stop offset="0" stop-color="#f00"/><stop offset="1" stop-color="#00f"/></linearGradient>
  <rect x="4" y="4" width="56" height="56" rx="8" fill="url(#g)"/>
  <path d="M8 40 C20 10 44 10 56 40" fill="none" stroke="#000" stroke-width="3"/>
</svg>)svg";

}  // namespace

/// Renders and validates one code-size smoke frame.
/// @param info Backend to instantiate and validate.
/// @return Zero on success, otherwise a diagnostic exit code.
int RunCodeSize(const donner::bakeoff::BackendInfo& info) {
  std::unique_ptr<donner::bakeoff::BenchBackend> backend = info.create();
  if (backend == nullptr) {
    return 3;
  }

  donner::ParseWarningSink warnings;
  auto parsed = donner::svg::parser::SVGParser::ParseSVG(kScene, warnings);
  if (parsed.hasError()) {
    return 4;
  }
  donner::svg::SVGDocument document = std::move(parsed.result());
  backend->renderer().draw(document);
  if (!backend->finishGpuWork()) {
    std::fprintf(stderr, "GPU completion failed\n");
    return 5;
  }

  const donner::svg::RendererBitmap bitmap = backend->renderer().takeSnapshot();
  if (bitmap.pixels.empty() && info.suite != donner::bakeoff::Suite::Reference) {
    std::fprintf(stderr, "renderer returned an empty snapshot\n");
    return 6;
  }
  std::uint32_t checksum = 2166136261u;
  for (const std::uint8_t byte : bitmap.pixels) {
    checksum = (checksum ^ byte) * 16777619u;
  }
  std::printf("%s %08x\n", info.name.c_str(), checksum);
  return 0;
}
int main() {
  const auto backends = donner::bakeoff::RegisteredBackends();
  if (backends.empty()) {
    std::fprintf(stderr, "no backend linked\n");
    return 2;
  }
  return RunCodeSize(backends.front());
}
