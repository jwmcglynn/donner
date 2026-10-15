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

int main() {
  const auto backends = donner::bakeoff::RegisteredBackends();
  if (backends.empty()) {
    std::fprintf(stderr, "no backend linked\n");
    return 2;
  }
  std::unique_ptr<donner::bakeoff::BenchBackend> backend = backends.front().create();
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
  backend->finishGpuWork();

  const donner::svg::RendererBitmap bitmap = backend->renderer().takeSnapshot();
  std::uint32_t checksum = 2166136261u;
  for (const std::uint8_t byte : bitmap.pixels) {
    checksum = (checksum ^ byte) * 16777619u;
  }
  std::printf("%s %08x\n", backends.front().name.c_str(), checksum);
  return 0;
}
