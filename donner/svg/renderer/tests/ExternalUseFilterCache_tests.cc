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
#include "donner/svg/SVGStyleElement.h"
#include "donner/svg/SVGUseElement.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/tests/ExternalUseSpriteScene.h"
#include "donner/svg/renderer/tests/RendererTestBackend.h"

namespace donner::svg::tests {
namespace {

/// The filter each sprite carries inside its own group.
enum class SpriteFilter {
  /// A Gaussian blur whose filter region leaves room for its whole reach, so its result is
  /// resampled at any sub-pixel offset.
  Blur,
  /// The same blur in the default filter region, which clips it to a hard edge, so its result is
  /// kept per quarter-pixel offset.
  BlurDefaultRegion,
  /// A blur merged under the sharp source graphic, kept per quarter-pixel offset.
  Glow,
};

constexpr int kHostWidth = 256;
constexpr int kHostHeight = 192;

/// A sheet of \p spriteCount sprites, each a filtered group of two shapes.
std::string MakeFilteredSheet(int spriteCount, SpriteFilter filter) {
  std::ostringstream out;
  out << R"(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">)";
  for (int sprite = 0; sprite < spriteCount; ++sprite) {
    out << "<g id=\"sprite-" << sprite << "\">";
    out << "<filter id=\"filter-" << sprite << "\"";
    if (filter == SpriteFilter::Blur) {
      out << R"( x="-150%" y="-150%" width="400%" height="400%")";
    } else if (filter == SpriteFilter::Glow) {
      out << R"( x="-50%" y="-50%" width="200%" height="200%")";
    }
    out << ">";
    if (filter != SpriteFilter::Glow) {
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
  // The first occurrence's miss only notes the content; the second records it (two-touch
  // admission), and every later occurrence on every frame reuses that result.
  EXPECT_EQ(total.executions, 2u);
  EXPECT_EQ(total.records, 1u);
  EXPECT_EQ(total.hits, static_cast<std::size_t>(kOccurrences * kFrames - 2));
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
                             const std::function<void(FilteredSpriteScene&)>& mutate,
                             const editor::tests::BitmapGoldenCompareParams& params =
                                 editor::tests::PixelmatchIdentityParams()) {
  std::unique_ptr<RendererInterface> renderer = MakeCachingRenderer();
  std::unique_ptr<RendererInterface> uncached = MakeCachingRenderer();
  uncached->setFilterResultCacheEnabled(false);
  PlaceAll(scene, 0, 1.0);
  std::size_t warmHits = 0;
  for (int frame = 0; frame < 2; ++frame) {
    warmHits += Draw(*renderer, scene).hits;
    (void)Draw(*uncached, scene);
  }
  EXPECT_GT(warmHits, 0u) << label << ": the warm frames must reuse a cached result";
  const RendererBitmap before = uncached->takeSnapshot();
  mutate(scene);
  const FrameWork work = Draw(*renderer, scene);
  (void)Draw(*uncached, scene);
  int changedPixels = 0;
  editor::tests::CompareBitmapToBitmap(
      uncached->takeSnapshot(), before, std::string(label) + "-mutation-visible",
      editor::tests::ApprovedPixelToleranceParams(0.0f, 1 << 30), &changedPixels);
  EXPECT_GT(changedPixels, 0) << label << ": the mutation must change the rendering";
  EXPECT_GE(work.executions, 1u) << label;
  editor::tests::CompareBitmapToBitmap(renderer->takeSnapshot(), uncached->takeSnapshot(), label,
                                       params);
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
  // Re-running the unscaled sprite's blur after another occurrence's larger, scaled filter ran in
  // the same frames moves a few dozen pixels by one level, cache or not, so the cached and
  // uncached frames are held to the suite's default per-pixel threshold here.
  ExpectMutationRefilters(
      *scene, "scaled-occurrence",
      [](FilteredSpriteScene& s) {
        s.uses[1].setTransform(Transform2d::Scale(1.5) * Transform2d::Translate(Vector2d(90, 60)));
      },
      editor::tests::ApprovedPixelToleranceParams(0.02f, 0));
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

// More distinct sprites than the cache holds, all drawn every frame, do not evict each other: the
// cache keeps what it holds and the rest render uncached. Sprites that stop being drawn are evicted
// for new ones, and drawing an evicted sprite again runs its filter again.
TEST(ExternalUseFilterCacheTest, CacheDoesNotThrashAndEvictedResultsRefilter) {
  REQUIRE_CACHING_BACKEND();
  constexpr int kSprites = 72;
  constexpr int kDrawn = 64;
  std::optional<FilteredSpriteScene> scene = MakeScene(kSprites, kSprites);
  ASSERT_TRUE(scene.has_value());
  // Every use shows a sprite starting at \p first; uses past kDrawn repeat earlier ones, so only
  // kDrawn distinct sprites are drawn.
  const auto showSprites = [&](int first) {
    for (int i = 0; i < kSprites; ++i) {
      const int sprite = (first + i % kDrawn) % kSprites;
      scene->uses[i].setHref(
          RcString(std::string(kExternalResourceUrl) + "#sprite-" + std::to_string(sprite)));
    }
  };
  std::unique_ptr<RendererInterface> renderer = MakeCachingRenderer();

  // All 72 sprites drawn every frame: the cache fills and then refuses to evict sprites in use.
  for (int i = 0; i < kSprites; ++i) {
    scene->uses[i].setHref(
        RcString(std::string(kExternalResourceUrl) + "#sprite-" + std::to_string(i)));
    scene->uses[i].setTransform(Transform2d::Translate(Vector2d((i % 12) * 20.0, (i / 12) * 30.0)));
  }
  FrameWork thrash;
  for (int frame = 0; frame < 4; ++frame) {
    const FrameWork work = Draw(*renderer, *scene);
    thrash.records += work.records;
    thrash.evictions += work.evictions;
  }
  EXPECT_LE(thrash.records, static_cast<std::size_t>(kDrawn));
  EXPECT_EQ(thrash.evictions, 0u);

  // Sprites 8..71 are drawn for a while; sprites 0..7 go stale and are evicted for them.
  showSprites(8);
  FrameWork shifted;
  for (int frame = 0; frame < 6; ++frame) {
    shifted.evictions += Draw(*renderer, *scene).evictions;
  }
  EXPECT_GT(shifted.evictions, 0u);

  // Sprite 0 comes back: its result was evicted, so its filter runs again.
  showSprites(0);
  EXPECT_GT(Draw(*renderer, *scene).executions, 0u);
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
  // A continuously changing scale never repeats a transform, so it records nothing.
  EXPECT_EQ(animating.records, 0u);
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

/// Render \p scene's single occurrence at \p translation with the cache disabled.
RendererBitmap RenderUncachedAt(FilteredSpriteScene& scene, Vector2d translation) {
  scene.uses[0].setTransform(Transform2d::Translate(translation));
  return RenderUncached(scene, RendererBackend::Geode);
}

/**
 * A result kept per quarter-pixel bucket is drawn on the pixel grid: a hit at a translation in the
 * recorded bucket is exactly the uncached render at the recorded translation moved by whole
 * pixels. Against an uncached render at the actual translation it differs only by the
 * sub-quarter-pixel quantization, which is bounded separately.
 */
void ExpectBucketedHitIsTheRecordedResultMoved(SpriteFilter filter, std::string_view label) {
  std::optional<FilteredSpriteScene> scene = MakeScene(1, 1, filter);
  ASSERT_TRUE(scene.has_value());
  const Vector2d recorded(10.1, 20.1);
  const Vector2d moved(13.15, 22.2);  // Same quarter-pixel bucket, (3, 2) whole pixels away.
  std::unique_ptr<RendererInterface> renderer = MakeCachingRenderer();
  scene->uses[0].setTransform(Transform2d::Translate(recorded));
  (void)Draw(*renderer, *scene);
  ASSERT_EQ(Draw(*renderer, *scene).records, 1u) << label;
  scene->uses[0].setTransform(Transform2d::Translate(moved));
  ASSERT_EQ(Draw(*renderer, *scene).hits, 1u) << label;
  const RendererBitmap cached = renderer->takeSnapshot();

  editor::tests::CompareBitmapToBitmap(
      cached, RenderUncachedAt(*scene, recorded + Vector2d(3.0, 2.0)),
      std::string(label) + "-recorded-placement", editor::tests::PixelmatchIdentityParams());
  // Edge pixels move by less than a quarter pixel; only anti-aliased edges may differ.
  editor::tests::CompareBitmapToBitmap(cached, RenderUncachedAt(*scene, moved),
                                       std::string(label) + "-quantization",
                                       editor::tests::ApprovedPixelToleranceParams(0.1f, 64));
}

TEST(ExternalUseFilterCacheTest, GlowHitIsTheRecordedResultMovedByWholePixels) {
  REQUIRE_CACHING_BACKEND();
  ExpectBucketedHitIsTheRecordedResultMoved(SpriteFilter::Glow, "glow");
}

// A blur clipped by the default filter region has a hard edge, so it is kept per bucket too.
TEST(ExternalUseFilterCacheTest, ClippedBlurHitIsTheRecordedResultMovedByWholePixels) {
  REQUIRE_CACHING_BACKEND();
  ExpectBucketedHitIsTheRecordedResultMoved(SpriteFilter::BlurDefaultRegion, "clipped-blur");
}

/// A scene from custom sources, with the uses `use-0` through `use-<occurrences - 1>`.
std::optional<FilteredSpriteScene> MakeCustomScene(std::string_view host, std::string sheet,
                                                   int occurrences) {
  std::optional<ExternalUseDocument> document = MakeExternalUseDocument(host, std::move(sheet));
  if (!document) {
    return std::nullopt;
  }
  FilteredSpriteScene scene{std::move(*document), {}};
  for (int i = 0; i < occurrences; ++i) {
    scene.uses.push_back(
        scene.host.document.querySelector("#use-" + std::to_string(i))->cast<SVGUseElement>());
  }
  return scene;
}

constexpr std::string_view kTwoUseHost = R"svg(
  <svg xmlns="http://www.w3.org/2000/svg" width="256" height="192">
    <rect width="100%" height="100%" fill="#f0f0e0"/>
    <use id="use-0" href="sprites.svg#sprite" width="32" height="32"/>
    <use id="use-1" href="sprites.svg#sprite" width="32" height="32"/>
  </svg>)svg";

TEST(ExternalUseFilterCacheTest, ResizingASymbolUseRefilters) {
  REQUIRE_CACHING_BACKEND();
  std::optional<FilteredSpriteScene> scene = MakeCustomScene(kTwoUseHost, R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
      <symbol id="sprite" viewBox="0 0 32 32">
        <filter id="f" x="-50%" y="-50%" width="200%" height="200%">
          <feGaussianBlur stdDeviation="2"/>
        </filter>
        <g filter="url(#f)"><circle cx="16" cy="16" r="8" fill="#c040a0"/></g>
      </symbol>
    </svg>)svg",
                                                             2);
  ASSERT_TRUE(scene.has_value());
  ExpectMutationRefilters(*scene, "symbol-viewport", [](FilteredSpriteScene& s) {
    s.uses[0].setAttribute("width", "48");
    s.uses[0].setAttribute("height", "48");
  });
}

TEST(ExternalUseFilterCacheTest, ChangingTheSheetStylesheetRefilters) {
  REQUIRE_CACHING_BACKEND();
  std::optional<FilteredSpriteScene> scene = MakeCustomScene(kTwoUseHost, R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
      <style id="css">.dot { fill: #c040a0; }</style>
      <g id="sprite">
        <filter id="f" x="-50%" y="-50%" width="200%" height="200%">
          <feGaussianBlur stdDeviation="2"/>
        </filter>
        <g filter="url(#f)"><circle class="dot" cx="16" cy="16" r="8"/></g>
      </g>
    </svg>)svg",
                                                             2);
  ASSERT_TRUE(scene.has_value());
  ExpectMutationRefilters(*scene, "sheet-stylesheet", [](FilteredSpriteScene& s) {
    s.sheet().querySelector("#css")->cast<SVGStyleElement>().setContents(".dot { fill: #20c040; }");
  });
}

TEST(ExternalUseFilterCacheTest, ChangingAGradientStopRefilters) {
  REQUIRE_CACHING_BACKEND();
  std::optional<FilteredSpriteScene> scene = MakeCustomScene(kTwoUseHost, R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
      <g id="sprite">
        <linearGradient id="paint">
          <stop id="first-stop" offset="0" stop-color="#ff0000"/>
          <stop offset="1" stop-color="#0000ff"/>
        </linearGradient>
        <filter id="f" x="-50%" y="-50%" width="200%" height="200%">
          <feGaussianBlur stdDeviation="2"/>
        </filter>
        <g filter="url(#f)"><rect x="4" y="4" width="24" height="24" fill="url(#paint)"/></g>
      </g>
    </svg>)svg",
                                                             2);
  ASSERT_TRUE(scene.has_value());
  ExpectMutationRefilters(*scene, "gradient-stop", [](FilteredSpriteScene& s) {
    s.sheet().querySelector("#first-stop")->setAttribute("stop-color", "#00ff00");
  });
}

// Drawing more sprites than the sheet's style retention keeps restyles the first one for its own
// document; when it is drawn again it is restyled for its occurrence under a new epoch, so its
// cached result is not reused.
TEST(ExternalUseFilterCacheTest, RetentionEvictionEndsTheCachedResult) {
  REQUIRE_CACHING_BACKEND();
  constexpr int kSprites = 36;
  std::optional<FilteredSpriteScene> scene = MakeScene(2, kSprites);
  ASSERT_TRUE(scene.has_value());
  const auto showSprite = [&](int sprite) {
    for (SVGUseElement& use : scene->uses) {
      use.setHref(
          RcString(std::string(kExternalResourceUrl) + "#sprite-" + std::to_string(sprite)));
    }
  };
  std::unique_ptr<RendererInterface> renderer = MakeCachingRenderer();
  std::unique_ptr<RendererInterface> uncached = MakeCachingRenderer();
  uncached->setFilterResultCacheEnabled(false);
  PlaceAll(*scene, 0, 1.0);
  showSprite(0);
  (void)Draw(*renderer, *scene);
  (void)Draw(*uncached, *scene);
  ASSERT_GT(Draw(*renderer, *scene).hits, 0u);
  (void)Draw(*uncached, *scene);
  for (int sprite = 1; sprite < kSprites; ++sprite) {
    showSprite(sprite);
    (void)Draw(*renderer, *scene);
    (void)Draw(*uncached, *scene);
  }
  showSprite(0);
  EXPECT_GT(Draw(*renderer, *scene).executions, 0u);
  (void)Draw(*uncached, *scene);
  editor::tests::CompareBitmapToBitmap(renderer->takeSnapshot(), uncached->takeSnapshot(),
                                       "retention-eviction",
                                       editor::tests::PixelmatchIdentityParams());
}

}  // namespace
}  // namespace donner::svg::tests
