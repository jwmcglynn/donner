#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string_view>

#include "donner/base/ParseWarningSink.h"
#include "donner/svg/SVG.h"
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"

namespace donner::svg {
namespace {

constexpr std::string_view kBoundaryMarker = "GEODE_GEOMETRY_BUDGET_BOUNDARY\n";

SVGDocument ParseRequiredDocument(std::string_view source) {
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(source, warnings);
  if (!parsed.hasResult()) std::abort();
  return std::move(parsed).result();
}

void RunBoundaryOracle(const std::shared_ptr<geode::GeodeDevice>& device) {
  SVGDocument document = ParseRequiredDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="8" height="4">
      <defs><linearGradient id="g"><stop stop-color="red"/></linearGradient></defs>
      <rect width="3" height="3"/>
      <rect x="4" width="3" height="3" fill="url(#g)"/>
    </svg>
  )svg");

  RendererGeode renderer(device);
  renderer.setGeometryBudgetForTesting(
      /*maximumDraws=*/1, std::numeric_limits<std::size_t>::max(),
      std::numeric_limits<std::uint64_t>::max(), std::numeric_limits<std::uint64_t>::max(),
      std::numeric_limits<std::uint64_t>::max());
  renderer.draw(document);
  const RendererResourceStats stats = renderer.resourceStats();
  if (!stats.geometryBudgetSupported || stats.geometryDraws != 1 || !stats.geometryBudgetRejected) {
    std::fprintf(stderr, "geometry boundary: supported=%d draws=%zu items=%zu rejected=%d\n",
                 stats.geometryBudgetSupported ? 1 : 0, stats.geometryDraws, stats.geometryItems,
                 stats.geometryBudgetRejected ? 1 : 0);
    std::abort();
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  static const std::shared_ptr<geode::GeodeDevice> device(geode::GeodeDevice::CreateHeadless());
  if (!device) return 0;

  const std::string_view input(reinterpret_cast<const char*>(data), size);  // NOLINT
  if (input == kBoundaryMarker) {
    RunBoundaryOracle(device);
    return 0;
  }

  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(input, warnings);
  if (!parsed.hasResult()) return 0;

  static RendererGeode renderer(device);
  SVGDocument document = std::move(parsed).result();
  renderer.draw(document);
  if (!renderer.resourceStats().geometryBudgetSupported) std::abort();
  return 0;
}

}  // namespace donner::svg
