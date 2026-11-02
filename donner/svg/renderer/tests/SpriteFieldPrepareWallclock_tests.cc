/// @file
/// Wall-clock cost of preparing a game frame: a 150-sprite field moving every frame, with
/// occasional fades and sprite flips, prepared with retained updates and with the full rebuild.
/// Reports the median host-document prepare time of each.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RendererUtils.h"
#include "donner/svg/renderer/tests/SpriteFieldScene.h"

namespace donner::svg::tests {
namespace {

constexpr int kWarmupFrames = 4;
constexpr int kMeasuredFrames = 40;

struct PrepareStats {
  double medianMs = 0.0;
  double meanMs = 0.0;
};

PrepareStats MeasurePrepare(bool retainedUpdates) {
  std::optional<SpriteFieldScene> scene = MakeSpriteFieldScene();
  if (!scene) {
    return {};
  }
  scene->document.registry().ctx().get<components::RenderTreeState>().retainedUpdatesEnabled =
      retainedUpdates;
  Renderer renderer;
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  std::vector<double> prepareMs;
  prepareMs.reserve(kMeasuredFrames);
  for (int frame = 0; frame < kWarmupFrames + kMeasuredFrames; ++frame) {
    AdvanceSpriteField(*scene, frame);
    const auto start = std::chrono::steady_clock::now();
    RendererUtils::prepareDocumentForRendering(scene->document, /*verbose=*/false, warnings);
    const auto end = std::chrono::steady_clock::now();
    // Drawing keeps the sheets' retained state as a game's frames leave it; it is not timed.
    renderer.draw(scene->document);
    if (frame >= kWarmupFrames) {
      prepareMs.push_back(std::chrono::duration<double, std::milli>(end - start).count());
    }
  }

  PrepareStats stats;
  for (const double ms : prepareMs) {
    stats.meanMs += ms / static_cast<double>(prepareMs.size());
  }
  std::nth_element(prepareMs.begin(), prepareMs.begin() + prepareMs.size() / 2, prepareMs.end());
  stats.medianMs = prepareMs[prepareMs.size() / 2];
  return stats;
}

TEST(SpriteFieldPrepareWallclockTest, ReportsPrepareTime) {
  const PrepareStats full = MeasurePrepare(/*retainedUpdates=*/false);
  const PrepareStats retained = MeasurePrepare(/*retainedUpdates=*/true);
  std::fprintf(stderr,
               "[SpriteFieldPrepare] 150 external uses, 7 sheets | full rebuild median=%.3fms "
               "mean=%.3fms | retained median=%.3fms mean=%.3fms\n",
               full.medianMs, full.meanMs, retained.medianMs, retained.meanMs);
  RecordProperty("fullRebuildMedianMs", std::to_string(full.medianMs));
  RecordProperty("retainedMedianMs", std::to_string(retained.medianMs));
  EXPECT_LT(retained.medianMs, full.medianMs);
}

}  // namespace
}  // namespace donner::svg::tests
