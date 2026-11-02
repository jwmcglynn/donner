/// @file
/// Wall-clock cost of filtered sprites drawn through external `<use>`, with and without cached
/// filter results: blurred background sprites moving every frame behind a few vector sprites, and
/// a grid of glowing pieces of a few kinds that move and briefly scale during swaps.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "donner/base/Transform.h"
#include "donner/svg/SVGUseElement.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/tests/ExternalUseSpriteScene.h"

namespace donner::svg::tests {
namespace {

constexpr int kWarmupFrames = 24;
constexpr int kMeasuredFrames = 120;

/// Cached frames stay below this fraction of uncached frames on a backend that caches.
constexpr double kMaximumCachedToUncachedRatio = 0.9;

struct FrameStats {
  double medianMs = 0.0;
  double meanMs = 0.0;
  double maxMs = 0.0;
};

/// A host document plus the uses an animation moves.
struct AnimatedScene {
  ExternalUseDocument host;
  std::vector<SVGUseElement> uses;
};

std::optional<AnimatedScene> MakeAnimatedScene(const std::string& hostSource,
                                               const std::string& sheetSource, int useCount) {
  std::optional<ExternalUseDocument> host = MakeExternalUseDocument(hostSource, sheetSource);
  if (!host) {
    return std::nullopt;
  }
  AnimatedScene scene{std::move(*host), {}};
  for (int i = 0; i < useCount; ++i) {
    scene.uses.push_back(scene.host.document.querySelector("#u" + std::to_string(i))
                             ->cast<SVGUseElement>());
  }
  return scene;
}

void AppendVectorShapes(std::ostringstream& out, int sprite, int count) {
  for (int i = 0; i < count; ++i) {
    const int x = 6 + (i * 37 + sprite * 11) % 60;
    const int y = 6 + (i * 53 + sprite * 7) % 60;
    out << "<circle cx=\"" << x << "\" cy=\"" << y << "\" r=\"" << 4 + i % 9 << "\" fill=\"#"
        << std::hex << ((0x305070 + i * 0x0a0b0c + sprite * 0x111111) & 0xFFFFFF) << std::dec
        << "\"/>";
  }
}

/// Twenty blurred background sprites drifting every frame behind three vector sprites, on a
/// 2x device-scale portrait canvas.
std::optional<AnimatedScene> MakeBackgroundBlurScene() {
  std::ostringstream sheet;
  sheet << R"(<svg xmlns="http://www.w3.org/2000/svg" width="80" height="80">)";
  for (int sprite = 0; sprite < 4; ++sprite) {
    sheet << "<g id=\"bg-" << sprite << "\"><filter id=\"bf-" << sprite
          << R"(" x="-50%" y="-50%" width="200%" height="200%">)"
          << R"(<feGaussianBlur stdDeviation="5.3"/></filter>)"
          << "<g filter=\"url(#bf-" << sprite << ")\">";
    AppendVectorShapes(sheet, sprite, 8);
    sheet << "</g></g>";
  }
  for (int sprite = 0; sprite < 3; ++sprite) {
    sheet << "<g id=\"fg-" << sprite << "\">";
    AppendVectorShapes(sheet, sprite + 4, 30);
    sheet << "</g>";
  }
  sheet << "</svg>";

  std::ostringstream host;
  host << R"(<svg xmlns="http://www.w3.org/2000/svg" width="800" height="1750" viewBox="0 0 400 875">)"
       << R"(<rect width="400" height="875" fill="#4a7bd0"/>)";
  for (int i = 0; i < 20; ++i) {
    host << "<use id=\"u" << i << "\" href=\"sprites.svg#bg-" << i % 4 << "\"/>";
  }
  for (int i = 20; i < 23; ++i) {
    host << "<use id=\"u" << i << "\" href=\"sprites.svg#fg-" << i - 20 << "\"/>";
  }
  host << "</svg>";
  return MakeAnimatedScene(host.str(), sheet.str(), 23);
}

void AdvanceBackgroundBlur(AnimatedScene& scene, int frame) {
  for (int i = 0; i < 20; ++i) {
    const double drift = (frame * (0.7 + 0.05 * i));
    scene.uses[i].setTransform(Transform2d::Translate(
        Vector2d(std::fmod(i * 37.0 + drift, 380.0), 20.0 + (i * 41) % 820)));
  }
  for (int i = 20; i < 23; ++i) {
    scene.uses[i].setTransform(
        Transform2d::Translate(Vector2d(60.0 + (frame % 60) * 2.0 + (i - 20) * 90.0, 600.0)));
  }
}

/// A 9x9 grid of glowing pieces of three kinds on a 3x device-scale portrait canvas. Pieces fall
/// by translation; every twelfth frame two neighbours swap, growing to 1.25x and back over six
/// frames.
std::optional<AnimatedScene> MakeGlowGridScene() {
  std::ostringstream sheet;
  sheet << R"(<svg xmlns="http://www.w3.org/2000/svg" width="40" height="40">)";
  for (int kind = 0; kind < 3; ++kind) {
    sheet << "<g id=\"piece-" << kind << "\"><filter id=\"glow-" << kind
          << R"(" x="-40%" y="-40%" width="180%" height="180%">)"
          << R"(<feGaussianBlur in="SourceGraphic" stdDeviation="2.5" result="halo"/>)"
          << R"(<feMerge><feMergeNode in="halo"/><feMergeNode in="SourceGraphic"/></feMerge>)"
          << "</filter><g filter=\"url(#glow-" << kind << ")\">";
    AppendVectorShapes(sheet, kind + 8, 6);
    sheet << "</g></g>";
  }
  sheet << "</svg>";

  std::ostringstream host;
  host << R"(<svg xmlns="http://www.w3.org/2000/svg" width="1206" height="2622" viewBox="0 0 402 874">)"
       << R"(<rect width="402" height="874" fill="#202040"/>)";
  for (int i = 0; i < 81; ++i) {
    host << "<use id=\"u" << i << "\" href=\"sprites.svg#piece-" << i % 3 << "\"/>";
  }
  host << "</svg>";
  return MakeAnimatedScene(host.str(), sheet.str(), 81);
}

void AdvanceGlowGrid(AnimatedScene& scene, int frame) {
  const int swapStart = frame - frame % 12;
  const int swapFrame = frame % 12;
  const int swapping = (swapStart / 12 * 7) % 80;
  for (int i = 0; i < 81; ++i) {
    const double fall = std::fmod(frame * 1.5 + (i % 9) * 3.0, 40.0);
    Transform2d transform =
        Transform2d::Translate(Vector2d(20.0 + (i % 9) * 42.0, 120.0 + (i / 9) * 70.0 + fall));
    if ((i == swapping || i == swapping + 1) && swapFrame < 6) {
      const double grow = 1.0 + 0.25 * (swapFrame < 3 ? swapFrame : 6 - swapFrame) / 3.0;
      transform = Transform2d::Scale(grow) * transform;
    }
    scene.uses[i].setTransform(transform);
  }
}

FrameStats MeasureFrames(const std::function<std::optional<AnimatedScene>()>& makeScene,
                         const std::function<void(AnimatedScene&, int)>& advance, bool cached,
                         bool* cacheSupported) {
  std::optional<AnimatedScene> scene = makeScene();
  if (!scene) {
    return {};
  }
  Renderer renderer;
  renderer.setFilterResultCacheEnabled(cached);
  for (int frame = 0; frame < kWarmupFrames; ++frame) {
    advance(*scene, frame);
    renderer.draw(scene->host.document);
  }
  *cacheSupported = renderer.resourceStats().filterResultCacheSupported;

  std::vector<double> frameMs;
  frameMs.reserve(kMeasuredFrames);
  for (int frame = kWarmupFrames; frame < kWarmupFrames + kMeasuredFrames; ++frame) {
    const auto start = std::chrono::steady_clock::now();
    advance(*scene, frame);
    renderer.draw(scene->host.document);
    // A frame is not done until its pixels are; read them back so GPU work is counted.
    (void)renderer.takeSnapshot();
    frameMs.push_back(
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
            .count());
  }
  FrameStats stats;
  for (double ms : frameMs) {
    stats.meanMs += ms / frameMs.size();
    stats.maxMs = std::max(stats.maxMs, ms);
  }
  std::nth_element(frameMs.begin(), frameMs.begin() + frameMs.size() / 2, frameMs.end());
  stats.medianMs = frameMs[frameMs.size() / 2];
  return stats;
}

void CompareCachedToUncached(const char* label,
                             const std::function<std::optional<AnimatedScene>()>& makeScene,
                             const std::function<void(AnimatedScene&, int)>& advance) {
  bool supported = false;
  const FrameStats uncached = MeasureFrames(makeScene, advance, /*cached=*/false, &supported);
  const FrameStats cached = MeasureFrames(makeScene, advance, /*cached=*/true, &supported);
  std::fprintf(stderr,
               "[%s] uncached median=%.3fms mean=%.3fms max=%.3fms | "
               "cached median=%.3fms mean=%.3fms max=%.3fms\n",
               label, uncached.medianMs, uncached.meanMs, uncached.maxMs, cached.medianMs,
               cached.meanMs, cached.maxMs);
  ::testing::Test::RecordProperty(std::string(label) + "UncachedMedianMs",
                                  std::to_string(uncached.medianMs));
  ::testing::Test::RecordProperty(std::string(label) + "CachedMedianMs",
                                  std::to_string(cached.medianMs));
  if (supported) {
    EXPECT_LT(cached.medianMs, uncached.medianMs * kMaximumCachedToUncachedRatio);
  }
}

TEST(ExternalUseFilterCacheWallclockTest, BlurredBackgroundSprites) {
  CompareCachedToUncached("BlurredBackground", MakeBackgroundBlurScene, AdvanceBackgroundBlur);
}

TEST(ExternalUseFilterCacheWallclockTest, GlowingGridWithSwaps) {
  CompareCachedToUncached("GlowingGrid", MakeGlowGridScene, AdvanceGlowGrid);
}

}  // namespace
}  // namespace donner::svg::tests
