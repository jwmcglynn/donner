/// @file
/// Wall-clock cost of a game frame: a 150-sprite field moving every frame, with occasional fades
/// and sprite flips. Reports the median host-document prepare time with retained updates and with
/// the full rebuild, and the per-frame time of the sprite sheets' font dependency collections.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/FontResourceGraph.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RendererDriver.h"
#include "donner/svg/renderer/RendererUtils.h"
#include "donner/svg/renderer/tests/MockRendererInterface.h"
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

/// Median, 90th percentile and mean per-frame time of the sprite sheets' font dependency
/// collections, plus the dependency graphs they built.
struct FontCollectionStats {
  double medianMs = 0.0;
  double p90Ms = 0.0;
  double meanMs = 0.0;
  std::size_t graphBuilds = 0;
};

/**
 * Times the font dependency collections a game frame makes: the renderer driver collects each
 * external use's target twice (complete and rendered-frame) after the use's sheet was prepared for
 * it, which invalidates the sheet's previous graph. The field is drawn once first so every sheet
 * has a render tree.
 */
FontCollectionStats MeasureFontCollections() {
  std::optional<SpriteFieldScene> scene = MakeSpriteFieldScene();
  if (!scene) {
    return {};
  }
  testing::NiceMock<MockRendererInterface> backend;
  RendererDriver driver(backend);
  driver.draw(scene->document);

  struct Collection {
    SVGDocumentHandle sheet;
    Entity target = entt::null;
  };
  std::vector<Collection> collections;
  for (SVGUseElement& use : scene->uses) {
    const auto* external = use.unsafeEntityHandle().try_get<components::ExternalUseComponent>();
    if (external == nullptr) {
      ADD_FAILURE() << "an external use has no loaded sheet";
      return {};
    }
    SVGDocument sheet = SVGDocument::CreateFromHandle(external->subDocument);
    const std::optional<SVGElement> sprite =
        sheet.querySelector("#" + std::string(external->fragment.str()));
    if (!sprite) {
      ADD_FAILURE() << "an external use's sprite is missing";
      return {};
    }
    collections.push_back({external->subDocument, sprite->unsafeEntityHandle().entity()});
  }

  std::vector<double> frameMs;
  frameMs.reserve(kMeasuredFrames);
  FontCollectionStats stats;
  for (int frame = 0; frame < kWarmupFrames + kMeasuredFrames; ++frame) {
    components::FontResourceGraphCache::Stats work;
    components::FontResourceGraphCache cache(&work);
    const auto start = std::chrono::steady_clock::now();
    for (const Collection& collection : collections) {
      components::InvalidateFontResourcePreparation(collection.sheet->registry());
      (void)cache.collect(collection.sheet, collection.target);
      (void)cache.collect(collection.sheet, collection.target,
                          components::FontResourceGraph::Purpose::RenderedFrame);
    }
    const auto end = std::chrono::steady_clock::now();
    if (frame >= kWarmupFrames) {
      frameMs.push_back(std::chrono::duration<double, std::milli>(end - start).count());
      stats.graphBuilds += work.graphBuilds;
    }
  }
  for (const double ms : frameMs) {
    stats.meanMs += ms / static_cast<double>(frameMs.size());
  }
  std::sort(frameMs.begin(), frameMs.end());
  stats.medianMs = frameMs[frameMs.size() / 2];
  stats.p90Ms = frameMs[frameMs.size() * 9 / 10];
  return stats;
}

TEST(SpriteFieldPrepareWallclockTest, ReportsSheetFontCollectionTime) {
  const FontCollectionStats stats = MeasureFontCollections();
  std::fprintf(stderr,
               "[SpriteFieldFontCollections] 150 external uses, 7 sheets | per frame median=%.3fms "
               "p90=%.3fms mean=%.3fms | graph builds per frame=%.1f\n",
               stats.medianMs, stats.p90Ms, stats.meanMs,
               static_cast<double>(stats.graphBuilds) / kMeasuredFrames);
  RecordProperty("fontCollectionMedianMs", std::to_string(stats.medianMs));
  EXPECT_GT(stats.medianMs, 0.0);
}

}  // namespace
}  // namespace donner::svg::tests
