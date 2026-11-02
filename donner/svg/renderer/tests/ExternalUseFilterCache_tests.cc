/// @file
/// Cached filter results for filtered sprites drawn through external `<use>`: a sprite's filter
/// runs once and its result is reused at every occurrence and on every frame that only moves it,
/// and runs again whenever anything that shapes the result changes.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/RcString.h"
#include "donner/base/Transform.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/SVGUseElement.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/tests/ExternalUseSpriteScene.h"
#include "donner/svg/renderer/tests/RendererTestBackend.h"

namespace donner::svg::tests {
namespace {

/// The filter each sprite carries inside its own group.
enum class SpriteFilter {
  Blur,  //!< A Gaussian blur, whose result is resampled at any sub-pixel offset.
  Glow,  //!< A blur merged under the sharp source graphic, kept per quarter-pixel offset.
};

constexpr int kHostWidth = 256;
constexpr int kHostHeight = 192;

/// A sheet of \p spriteCount sprites, each a filtered group of two shapes.
std::string MakeFilteredSheet(int spriteCount, SpriteFilter filter) {
  std::ostringstream out;
  out << R"(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">)";
  for (int sprite = 0; sprite < spriteCount; ++sprite) {
    out << "<g id=\"sprite-" << sprite << "\">";
    out << "<filter id=\"filter-" << sprite << R"(" x="-50%" y="-50%" width="200%" height="200%">)";
    if (filter == SpriteFilter::Blur) {
      out << "<feGaussianBlur id=\"blur-" << sprite << "\" stdDeviation=\"5.3\"/>";
    } else {
      out << "<feGaussianBlur id=\"blur-" << sprite
          << R"(" in="SourceGraphic" stdDeviation="3" result="halo"/>)"
          << R"(<feMerge><feMergeNode in="halo"/><feMergeNode in="SourceGraphic"/></feMerge>)";
    }
    out << "</filter>";
    out << "<g filter=\"url(#filter-" << sprite << ")\">";
    out << "<circle id=\"dot-" << sprite << "\" cx=\"16\" cy=\"16\" r=\"" << 6 + sprite % 5
        << R"(" fill="#c040a0"/>)";
    out << R"(<rect x="8" y="20" width="16" height="6" fill="currentColor"/>)";
    out << "</g></g>";
  }
  out << "</svg>";
  return out.str();
}

/// A host drawing \p occurrences uses of `sprite-0`, ids `use-0` onwards.
std::string MakeHost(int occurrences) {
  std::ostringstream out;
  out << R"(<svg xmlns="http://www.w3.org/2000/svg" width=")" << kHostWidth << "\" height=\""
      << kHostHeight << R"(" color="#30a0e0"><rect width="100%" height="100%" fill="#f0f0e0"/>)";
  for (int i = 0; i < occurrences; ++i) {
    out << "<use id=\"use-" << i << "\" href=\"sprites.svg#sprite-0\"/>";
  }
  out << "</svg>";
  return out.str();
}

/// A scene of filtered sprite occurrences and a renderer for the backend under test.
struct FilteredSpriteScene {
  ExternalUseDocument host;
  std::vector<SVGUseElement> uses;

  /// The sprite sheet, once a draw has loaded it.
  SVGDocument sheet() const { return SVGDocument::CreateFromHandle(*host.resource); }
};

std::optional<FilteredSpriteScene> MakeScene(int occurrences, int spriteCount = 1,
                                             SpriteFilter filter = SpriteFilter::Blur) {
  std::optional<ExternalUseDocument> host =
      MakeExternalUseDocument(MakeHost(occurrences), MakeFilteredSheet(spriteCount, filter));
  if (!host) {
    return std::nullopt;
  }
  FilteredSpriteScene scene{std::move(*host), {}};
  for (int i = 0; i < occurrences; ++i) {
    scene.uses.push_back(
        scene.host.document.querySelector("#use-" + std::to_string(i))->cast<SVGUseElement>());
  }
  return scene;
}

/// Places occurrence \p index for \p frame; \p step is the per-frame motion in pixels.
void PlaceOccurrence(FilteredSpriteScene& scene, int index, int frame, double step) {
  scene.uses[index].setTransform(Transform2d::Translate(
      Vector2d(10.0 + index * 44.0 + frame * step, 20.0 + (index % 3) * 50.0 + frame * step)));
}

void PlaceAll(FilteredSpriteScene& scene, int frame, double step) {
  for (int i = 0; i < static_cast<int>(scene.uses.size()); ++i) {
    PlaceOccurrence(scene, i, frame, step);
  }
}

/// Per-frame filter work a backend reported.
struct FrameWork {
  std::size_t executions = 0;
  std::size_t hits = 0;
  std::size_t records = 0;
  std::size_t evictions = 0;
};

FrameWork Draw(RendererInterface& renderer, FilteredSpriteScene& scene) {
  renderer.draw(scene.host.document);
  const RendererResourceStats stats = renderer.resourceStats();
  return {stats.filterExecutions, stats.filterResultCacheHits, stats.filterResultCacheRecords,
          stats.filterResultCacheEvictions};
}

/// Skips unless this binary links a backend that caches filter results and runs filters.
#define REQUIRE_CACHING_BACKEND()                                                               \
  do {                                                                                          \
    if (!IsRendererBackendAvailable(RendererBackend::Geode) ||                                  \
        !RendererBackendSupportsFeature(RendererBackend::Geode,                                 \
                                        RendererBackendFeature::FilterEffects)) {               \
      GTEST_SKIP() << "Filter result caching is implemented by the Geode backend with filters"; \
    }                                                                                           \
  } while (false)

std::unique_ptr<RendererInterface> MakeCachingRenderer() {
  return CreateRendererInstance(RendererBackend::Geode);
}

/// The backend this binary renders with by default, for comparisons that hold on every backend.
RendererBackend DefaultBackend() {
  return IsRendererBackendAvailable(RendererBackend::Geode) ? RendererBackend::Geode
                                                            : RendererBackend::TinySkia;
}

/// Warm \p renderer on \p scene at \p frame, so the next draw can reuse cached results.
void Warm(RendererInterface& renderer, FilteredSpriteScene& scene, int frame) {
  PlaceAll(scene, frame, 1.0);
  (void)Draw(renderer, scene);
  (void)Draw(renderer, scene);
}

/// Render the scene's current state with a fresh renderer whose cache is disabled.
RendererBitmap RenderUncached(FilteredSpriteScene& scene, RendererBackend backend) {
  std::unique_ptr<RendererInterface> renderer = CreateRendererInstance(backend);
  renderer->setFilterResultCacheEnabled(false);
  renderer->draw(scene.host.document);
  return renderer->takeSnapshot();
}

TEST(ExternalUseFilterCacheTest, FilterRunsOnceAcrossFramesAndOccurrences) {
  REQUIRE_CACHING_BACKEND();
  constexpr int kOccurrences = 5;
  constexpr int kFrames = 8;
  std::optional<FilteredSpriteScene> scene = MakeScene(kOccurrences);
  ASSERT_TRUE(scene.has_value());
  std::unique_ptr<RendererInterface> renderer = MakeCachingRenderer();

  FrameWork total;
  for (int frame = 0; frame < kFrames; ++frame) {
    // Alternate integer and fractional motion so both kinds of translation reuse the result.
    PlaceAll(*scene, frame, frame % 2 == 0 ? 2.0 : 1.25);
    const FrameWork work = Draw(*renderer, *scene);
    total.executions += work.executions;
    total.hits += work.hits;
    total.records += work.records;
  }
  EXPECT_EQ(total.executions, 1u);
  EXPECT_EQ(total.records, 1u);
  EXPECT_EQ(total.hits, static_cast<std::size_t>(kOccurrences * kFrames - 1));
}

TEST(ExternalUseFilterCacheTest, DisabledCacheRunsTheFilterForEveryOccurrence) {
  REQUIRE_CACHING_BACKEND();
  constexpr int kOccurrences = 3;
  std::optional<FilteredSpriteScene> scene = MakeScene(kOccurrences);
  ASSERT_TRUE(scene.has_value());
  std::unique_ptr<RendererInterface> renderer = MakeCachingRenderer();
  renderer->setFilterResultCacheEnabled(false);
  for (int frame = 0; frame < 3; ++frame) {
    PlaceAll(*scene, frame, 1.0);
    const FrameWork work = Draw(*renderer, *scene);
    EXPECT_EQ(work.executions, static_cast<std::size_t>(kOccurrences));
    EXPECT_EQ(work.hits, 0u);
  }
}

/// A mutation of the scene after warm frames runs the filter again and renders like a renderer
/// with the cache disabled that drew the same frames. The uncached oracle repeats the history
/// rather than rendering fresh, so only the cache differs between the two.
void ExpectMutationRefilters(FilteredSpriteScene& scene, std::string_view label,
                             const std::function<void(FilteredSpriteScene&)>& mutate) {
  std::unique_ptr<RendererInterface> renderer = MakeCachingRenderer();
  std::unique_ptr<RendererInterface> uncached = MakeCachingRenderer();
  uncached->setFilterResultCacheEnabled(false);
  PlaceAll(scene, 0, 1.0);
  for (int frame = 0; frame < 2; ++frame) {
    (void)Draw(*renderer, scene);
    (void)Draw(*uncached, scene);
  }
  mutate(scene);
  const FrameWork work = Draw(*renderer, scene);
  (void)Draw(*uncached, scene);
  EXPECT_GE(work.executions, 1u) << label;
  editor::tests::CompareBitmapToBitmap(renderer->takeSnapshot(), uncached->takeSnapshot(), label,
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(ExternalUseFilterCacheTest, MutatingTheSpriteRefilters) {
  REQUIRE_CACHING_BACKEND();
  std::optional<FilteredSpriteScene> scene = MakeScene(2);
  ASSERT_TRUE(scene.has_value());
  ExpectMutationRefilters(*scene, "mutated-sprite", [](FilteredSpriteScene& s) {
    s.sheet().querySelector("#dot-0")->setAttribute("fill", "#20c040");
  });
}

TEST(ExternalUseFilterCacheTest, ChangingStdDeviationRefilters) {
  REQUIRE_CACHING_BACKEND();
  std::optional<FilteredSpriteScene> scene = MakeScene(2);
  ASSERT_TRUE(scene.has_value());
  ExpectMutationRefilters(*scene, "changed-std-deviation", [](FilteredSpriteScene& s) {
    s.sheet().querySelector("#blur-0")->setAttribute("stdDeviation", "2");
  });
}

TEST(ExternalUseFilterCacheTest, ScalingAnOccurrenceRefilters) {
  REQUIRE_CACHING_BACKEND();
  std::optional<FilteredSpriteScene> scene = MakeScene(2);
  ASSERT_TRUE(scene.has_value());
  ExpectMutationRefilters(*scene, "scaled-occurrence", [](FilteredSpriteScene& s) {
    s.uses[1].setTransform(Transform2d::Scale(1.5) * Transform2d::Translate(Vector2d(90, 60)));
  });
}

TEST(ExternalUseFilterCacheTest, RotatingAnOccurrenceRefilters) {
  REQUIRE_CACHING_BACKEND();
  std::optional<FilteredSpriteScene> scene = MakeScene(2);
  ASSERT_TRUE(scene.has_value());
  ExpectMutationRefilters(*scene, "rotated-occurrence", [](FilteredSpriteScene& s) {
    s.uses[1].setTransform(Transform2d::Rotate(0.3) * Transform2d::Translate(Vector2d(120, 40)));
  });
}

TEST(ExternalUseFilterCacheTest, ChangingTheDeviceScaleRefilters) {
  REQUIRE_CACHING_BACKEND();
  std::optional<FilteredSpriteScene> scene = MakeScene(2);
  ASSERT_TRUE(scene.has_value());
  ExpectMutationRefilters(*scene, "device-scale", [](FilteredSpriteScene& s) {
    s.host.document.setCanvasSize(kHostWidth * 2, kHostHeight * 2);
  });
}

TEST(ExternalUseFilterCacheTest, RetargetingTheUseRefilters) {
  REQUIRE_CACHING_BACKEND();
  std::optional<FilteredSpriteScene> scene = MakeScene(2, /*spriteCount=*/2);
  ASSERT_TRUE(scene.has_value());
  ExpectMutationRefilters(*scene, "retargeted-use", [](FilteredSpriteScene& s) {
    s.uses[0].setHref(RcString(std::string(kExternalResourceUrl) + "#sprite-1"));
  });
}

TEST(ExternalUseFilterCacheTest, HostStyleChangeRefilters) {
  REQUIRE_CACHING_BACKEND();
  std::optional<FilteredSpriteScene> scene = MakeScene(2);
  ASSERT_TRUE(scene.has_value());
  ExpectMutationRefilters(*scene, "host-style", [](FilteredSpriteScene& s) {
    // The sprite's bar is painted with currentColor, which inherits from the use.
    s.host.document.querySelector("#use-0")->setStyle("color: #e0a020");
  });
}

TEST(ExternalUseFilterCacheTest, EvictedResultsRefilter) {
  REQUIRE_CACHING_BACKEND();
  // More distinct sprites than the cache holds, all drawn in one frame, evict the earliest.
  constexpr int kSprites = 72;
  std::optional<FilteredSpriteScene> scene = MakeScene(kSprites, kSprites);
  ASSERT_TRUE(scene.has_value());
  for (int i = 0; i < kSprites; ++i) {
    scene->uses[i].setHref(
        RcString(std::string(kExternalResourceUrl) + "#sprite-" + std::to_string(i)));
    scene->uses[i].setTransform(Transform2d::Translate(Vector2d((i % 12) * 20.0, (i / 12) * 30.0)));
  }
  std::unique_ptr<RendererInterface> renderer = MakeCachingRenderer();
  const FrameWork first = Draw(*renderer, *scene);
  EXPECT_EQ(first.records, static_cast<std::size_t>(kSprites));
  EXPECT_GT(first.evictions, 0u);
  EXPECT_LE(renderer->resourceStats().filterResultCacheEntries,
            static_cast<std::size_t>(kSprites) - first.evictions);

  const FrameWork second = Draw(*renderer, *scene);
  EXPECT_GT(second.executions, 0u);
  EXPECT_EQ(second.hits + second.executions, static_cast<std::size_t>(kSprites));
}

TEST(ExternalUseFilterCacheTest, ScaleAnimationDoesNotRecordEveryFrame) {
  REQUIRE_CACHING_BACKEND();
  std::optional<FilteredSpriteScene> scene = MakeScene(3);
  ASSERT_TRUE(scene.has_value());
  std::unique_ptr<RendererInterface> renderer = MakeCachingRenderer();
  Warm(*renderer, *scene, 0);

  // Occurrence 0 grows from 1.0 to 1.25 over ten frames while the others hold still.
  FrameWork animating;
  for (int frame = 1; frame <= 10; ++frame) {
    scene->uses[0].setTransform(Transform2d::Scale(1.0 + 0.025 * frame) *
                                Transform2d::Translate(Vector2d(10, 20)));
    const FrameWork work = Draw(*renderer, *scene);
    animating.records += work.records;
    animating.hits += work.hits;
  }
  // The scale change's first frame may record; a continuously changing scale records no more.
  EXPECT_LE(animating.records, 1u);
  EXPECT_EQ(animating.hits, 20u);

  // Once the scale holds, its result is recorded once and reused.
  FrameWork settled;
  for (int frame = 0; frame < 4; ++frame) {
    const FrameWork work = Draw(*renderer, *scene);
    settled.records += work.records;
    settled.hits += work.hits;
  }
  EXPECT_EQ(settled.records, 1u);
  EXPECT_GE(settled.hits, 10u);
}

/// Cached frames match an uncached render of the same state within the documented tolerance.
void ExpectCachedMatchesUncached(SpriteFilter filter, double step, std::string_view label,
                                 const editor::tests::BitmapGoldenCompareParams& params) {
  constexpr int kOccurrences = 4;
  std::optional<FilteredSpriteScene> scene = MakeScene(kOccurrences, 1, filter);
  ASSERT_TRUE(scene.has_value());
  const RendererBackend backend = DefaultBackend();
  std::unique_ptr<RendererInterface> renderer = CreateRendererInstance(backend);
  for (int frame = 0; frame < 4; ++frame) {
    PlaceAll(*scene, frame, step);
    renderer->draw(scene->host.document);
  }
  if (backend == RendererBackend::Geode) {
    EXPECT_GT(renderer->resourceStats().filterResultCacheHits, 0u) << label;
  }
  editor::tests::CompareBitmapToBitmap(renderer->takeSnapshot(), RenderUncached(*scene, backend),
                                       label, params);
}

// Integer motion keeps every cached result on the pixel grid it was recorded on, so both filters
// match exactly.
TEST(ExternalUseFilterCacheTest, IntegerMotionMatchesUncachedBlur) {
  ExpectCachedMatchesUncached(SpriteFilter::Blur, 3.0, "integer-blur",
                              editor::tests::PixelmatchIdentityParams());
}

TEST(ExternalUseFilterCacheTest, IntegerMotionMatchesUncachedGlow) {
  ExpectCachedMatchesUncached(SpriteFilter::Glow, 3.0, "integer-glow",
                              editor::tests::PixelmatchIdentityParams());
}

// A blurred result is resampled bilinearly at the new sub-pixel offset. Resampling a result whose
// detail is already low-passed by a sigma of at least one pixel moves each channel by a few
// levels at most.
TEST(ExternalUseFilterCacheTest, FractionalMotionMatchesUncachedBlur) {
  ExpectCachedMatchesUncached(SpriteFilter::Blur, 1.37, "fractional-blur",
                              editor::tests::ApprovedPixelToleranceParams(0.02f, 0));
}

// A glow keeps the sharp source, so its result is reused only within a quarter-pixel bucket and
// drawn on the pixel grid: anti-aliased edge pixels shift by less than a quarter pixel.
TEST(ExternalUseFilterCacheTest, FractionalMotionMatchesUncachedGlow) {
  ExpectCachedMatchesUncached(SpriteFilter::Glow, 1.37, "fractional-glow",
                              editor::tests::ApprovedPixelToleranceParams(0.1f, 160));
}

}  // namespace
}  // namespace donner::svg::tests
