/// @file
/// Wall-clock cost of an animated external-use sprite: per-frame draw time with bitmap sprites
/// versus vector sprites from the same kind of sprite sheet, through the configured backend.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/tests/ExternalUseSpriteScene.h"

namespace donner::svg::tests {
namespace {

constexpr int kWarmupFrames = 2 * kFramesPerSpriteChange * kSpriteSheetSpriteCount;
constexpr int kMeasuredFrames = 240;

/// Vector sprites stay within this multiple of the bitmap sprites' median frame time. The scene's
/// vector sheet holds 540 paths; a frame draws two sprites of 45 paths each.
constexpr double kMaximumVectorToBitmapRatio = 3.0;

struct FrameStats {
  double medianMs = 0.0;
  double meanMs = 0.0;
  double maxMs = 0.0;
};

FrameStats MeasureFrames(SpriteKind kind) {
  std::optional<ExternalUseSpriteScene> scene = MakeExternalUseSpriteScene(kind);
  if (!scene) {
    return {};
  }
  Renderer renderer;
  for (int frame = 0; frame < kWarmupFrames; ++frame) {
    AdvanceExternalUseSpriteFrame(*scene, frame);
    renderer.draw(scene->host.document);
  }

  std::vector<double> frameMs;
  frameMs.reserve(kMeasuredFrames);
  for (int frame = kWarmupFrames; frame < kWarmupFrames + kMeasuredFrames; ++frame) {
    const auto start = std::chrono::steady_clock::now();
    AdvanceExternalUseSpriteFrame(*scene, frame);
    renderer.draw(scene->host.document);
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

TEST(ExternalUseSpriteWallclockTest, VectorSpritesCostComparableToBitmapSprites) {
  const FrameStats bitmap = MeasureFrames(SpriteKind::Bitmap);
  const FrameStats vector = MeasureFrames(SpriteKind::Vector);
  std::fprintf(stderr,
               "[ExternalUseSprite] bitmap median=%.3fms mean=%.3fms max=%.3fms | "
               "vector median=%.3fms mean=%.3fms max=%.3fms\n",
               bitmap.medianMs, bitmap.meanMs, bitmap.maxMs, vector.medianMs, vector.meanMs,
               vector.maxMs);
  RecordProperty("bitmapMedianMs", std::to_string(bitmap.medianMs));
  RecordProperty("vectorMedianMs", std::to_string(vector.medianMs));
  EXPECT_LT(vector.medianMs, bitmap.medianMs * kMaximumVectorToBitmapRatio);
}

}  // namespace
}  // namespace donner::svg::tests
