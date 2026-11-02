/// @file
/// Wall-clock and surface cost of many small filtered elements on a large canvas through Geode: a
/// 1206x2622 phone-sized scene of about a hundred drifting blurred pieces of five kinds, eighty
/// glowing pieces of three kinds, and a few lit bevels.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"

namespace donner::svg {
namespace {

constexpr int kWidth = 1206;
constexpr int kHeight = 2622;
constexpr int kBackgroundPieces = 100;  // Five kinds, an inner glow then a 5.3 px blur each.
constexpr int kGlowingPieces = 80;      // Three kinds, a blur merged under the source each.
constexpr int kBevels = 4;              // Lighting-filter bevels.
constexpr int kPieces = kBackgroundPieces + kGlowingPieces + kBevels;
constexpr int kWarmupFrames = 10;
constexpr int kMeasuredFrames = 60;

std::string PieceTransform(int index, int frame) {
  if (index < kBackgroundPieces) {
    const double x = 30.0 + (index % 10) * 118.0 + std::fmod(frame * 0.7 + index * 3.0, 40.0);
    const double y = 60.0 + (index / 10) * 250.0 + std::fmod(frame * 1.3, 30.0);
    return "translate(" + std::to_string(x) + " " + std::to_string(y) + ")";
  }
  const int glowing = index - kBackgroundPieces;
  const double x = 70.0 + (glowing % 9) * 126.0;
  const double y = 120.0 + (glowing / 9) * 270.0 + (frame * 1.5 + (glowing % 9) * 3.0);
  return "translate(" + std::to_string(x) + " " + std::to_string(y) + ")";
}

/// A phone-sized scene: drifting blurred background pieces, a grid of glowing candies, and a few
/// lit bevels, all filtered.
std::string FilteredSceneSource() {
  std::string source = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" +
                       std::to_string(kWidth) + "\" height=\"" + std::to_string(kHeight) + "\">";
  source += R"svg(<rect width="100%" height="100%" fill="#202040"/>
    <filter id="glow" x="-40%" y="-40%" width="180%" height="180%">
      <feGaussianBlur in="SourceGraphic" stdDeviation="7.5" result="halo"/>
      <feMerge><feMergeNode in="halo"/><feMergeNode in="SourceGraphic"/></feMerge>
    </filter>
    <filter id="soft" x="-30%" y="-30%" width="160%" height="160%">
      <feGaussianBlur in="SourceAlpha" stdDeviation="4" result="inset"/>
      <feComposite in="SourceAlpha" in2="inset" operator="out" result="edge"/>
      <feFlood flood-color="#ffffff" flood-opacity="0.6"/>
      <feComposite in2="edge" operator="in" result="rim"/>
      <feMerge result="lit"><feMergeNode in="SourceGraphic"/><feMergeNode in="rim"/></feMerge>
      <feGaussianBlur in="lit" stdDeviation="5.3"/>
    </filter>
    <filter id="bevel" x="-20%" y="-20%" width="140%" height="140%">
      <feGaussianBlur in="SourceAlpha" stdDeviation="3" result="height"/>
      <feDiffuseLighting in="height" surfaceScale="4" lighting-color="#ffffff" result="light">
        <feDistantLight azimuth="225" elevation="45"/>
      </feDiffuseLighting>
      <feComposite in="light" in2="SourceAlpha" operator="in" result="shade"/>
      <feBlend in="SourceGraphic" in2="shade" mode="multiply"/>
    </filter>)svg";
  const char* kBackgroundFills[] = {"#305070", "#507030", "#703050", "#406080", "#806040"};
  const char* kGlowFills[] = {"#e05090", "#40c0f0", "#f0c040"};
  for (int i = 0; i < kPieces; ++i) {
    const std::string id = "p" + std::to_string(i);
    if (i < kBackgroundPieces) {
      source += "<g id=\"" + id + "\" filter=\"url(#soft)\" transform=\"" + PieceTransform(i, 0) +
                "\"><rect width=\"" + std::to_string(50 + (i % 5) * 8) +
                "\" height=\"40\" rx=\"8\" fill=\"" + kBackgroundFills[i % 5] + "\"/></g>";
    } else if (i < kBackgroundPieces + kGlowingPieces) {
      source += "<g id=\"" + id + "\" filter=\"url(#glow)\" transform=\"" + PieceTransform(i, 0) +
                "\"><circle r=\"44\" fill=\"" + kGlowFills[i % 3] +
                "\"/><rect x=\"-14\" y=\"-60\" width=\"28\" height=\"24\" fill=\"#ffffff\"/></g>";
    } else {
      const int bevel = i - kBackgroundPieces - kGlowingPieces;
      source += "<g id=\"" + id + "\" filter=\"url(#bevel)\" transform=\"translate(" +
                std::to_string(200 + bevel * 220) +
                " 2450)\"><rect width=\"120\" height=\"60\" rx=\"12\" fill=\"#c08040\"/></g>";
    }
  }
  return source + "</svg>";
}

TEST(GeodeFilterSurfacesWallclockTest, FilteredGameSceneOnAPhoneCanvas) {
  std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
  ASSERT_NE(device, nullptr);
  RendererGeode renderer(device);

  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(FilteredSceneSource(), warnings);
  ASSERT_FALSE(parsed.hasError());
  SVGDocument document = std::move(parsed.result());
  std::vector<SVGElement> pieces;
  for (int i = 0; i < kPieces; ++i) {
    pieces.push_back(*document.querySelector("#p" + std::to_string(i)));
  }

  std::vector<double> frameMs;
  std::uint64_t peakSurfaceBytes = 0;
  std::uint64_t textureCreates = 0;
  std::uint64_t filterExecutions = 0;
  for (int frame = 0; frame < kWarmupFrames + kMeasuredFrames; ++frame) {
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kBackgroundPieces + kGlowingPieces; ++i) {
      pieces[i].setAttribute("transform", PieceTransform(i, frame));
    }
    renderer.draw(document);
    // A frame is not done until its pixels are; read them back so GPU work is counted.
    (void)renderer.takeSnapshot();
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (frame >= kWarmupFrames) {
      frameMs.push_back(ms);
      peakSurfaceBytes = std::max(peakSurfaceBytes, renderer.resourceStats().surfacePeakBytes);
      textureCreates += renderer.lastFrameTimings().counters.textureCreates;
      filterExecutions += renderer.resourceStats().filterExecutions;
    }
  }
  std::nth_element(frameMs.begin(), frameMs.begin() + frameMs.size() / 2, frameMs.end());
  const double medianMs = frameMs[frameMs.size() / 2];
  const RendererGeodeTexturePoolStats pool = renderer.texturePoolStats();
  std::fprintf(stderr,
               "[GeodeFilterSurfaces] median=%.3fms filtersRunPerFrame=%.1f of %d "
               "textureCreatesPerFrame=%.2f peakSurfaceMiB=%.2f poolMiB=%.2f poolTextures=%zu\n",
               medianMs, static_cast<double>(filterExecutions) / kMeasuredFrames, kPieces,
               static_cast<double>(textureCreates) / kMeasuredFrames,
               static_cast<double>(peakSurfaceBytes) / (1 << 20),
               static_cast<double>(pool.bytes) / (1 << 20), pool.textureCount);
  RecordProperty("medianMs", std::to_string(medianMs));
  RecordProperty("peakSurfaceBytes", std::to_string(peakSurfaceBytes));
  RecordProperty("textureCreates", std::to_string(textureCreates));
}

}  // namespace
}  // namespace donner::svg
