/// @file
/// Cached sprites: an external `<use>` occurrence is rasterized once, whole, and later occurrences
/// and frames that only move it draw that rasterization instead of the sheet's content.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <utility>
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

constexpr int kHostWidth = 256;
constexpr int kHostHeight = 192;

/// A sheet whose sprites exercise what a cached sprite may hold: a gradient fill under a clip
/// path, an opacity group with `currentColor` and a stroke, and a blurred group.
constexpr std::string_view kSheet = R"svg(
  <svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
    <defs>
      <linearGradient id="grad"><stop offset="0" stop-color="#f04060"/>
        <stop offset="1" stop-color="#4060f0"/></linearGradient>
      <clipPath id="clip"><circle cx="16" cy="16" r="13"/></clipPath>
      <filter id="blur" x="-50%" y="-50%" width="200%" height="200%">
        <feGaussianBlur id="blur-node" stdDeviation="1.5"/>
      </filter>
    </defs>
    <g id="sprite-0">
      <rect id="body-0" x="4" y="4" width="24" height="24" fill="url(#grad)" clip-path="url(#clip)"/>
      <g opacity="0.6"><circle cx="16" cy="16" r="6" fill="currentColor" stroke="#202020"
        stroke-width="2"/></g>
      <g filter="url(#blur)"><rect x="10" y="22" width="12" height="4" fill="#ffd040"/></g>
    </g>
    <g id="sprite-1">
      <path d="M4 16 C4 6 28 6 28 16 C28 26 4 26 4 16 Z" fill="#30c060"/>
    </g>
    <symbol id="symbol" viewBox="0 0 32 32">
      <circle cx="16" cy="16" r="12" fill="#c040a0"/>
    </symbol>
    <filter id="wide" x="-150%" y="-150%" width="400%" height="400%">
      <feGaussianBlur stdDeviation="3"/>
    </filter>
    <g id="blurred"><g filter="url(#wide)"><circle cx="16" cy="16" r="8" fill="#8040c0"/>
      <rect x="10" y="20" width="12" height="4" fill="#40c080"/></g></g>
    <g id="inheritor" style="opacity: inherit"><rect width="20" height="20" fill="#e06020"/></g>
    <symbol id="blursym" viewBox="0 0 16 16">
      <g filter="url(#blur)"><circle cx="8" cy="8" r="4" fill="#2060e0"/></g>
      <rect x="2" y="2" width="4" height="4" fill="#e0e020"/>
    </symbol>
    <g id="imaged"><image width="8" height="8" href="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg=="/></g>
    <filter id="distant" x="-10%" y="-10%" width="120%" height="120%">
      <feGaussianBlur in="SourceAlpha" stdDeviation="2" result="height"/>
      <feDiffuseLighting in="height" surfaceScale="3" result="light">
        <feDistantLight azimuth="225" elevation="45"/></feDiffuseLighting>
      <feComposite in="light" in2="SourceAlpha" operator="in"/>
    </filter>
    <filter id="spot" x="-10%" y="-10%" width="120%" height="120%">
      <feGaussianBlur in="SourceAlpha" stdDeviation="2" result="height"/>
      <feDiffuseLighting in="height" surfaceScale="3" result="light">
        <fePointLight x="10" y="10" z="20"/></feDiffuseLighting>
      <feComposite in="light" in2="SourceAlpha" operator="in"/>
    </filter>
    <g id="lit"><g filter="url(#distant)"><rect x="2" y="2" width="28" height="16" rx="4"
      fill="#c08040"/></g></g>
    <g id="pointlit"><g filter="url(#spot)"><rect x="2" y="2" width="28" height="16" rx="4"
      fill="#c08040"/></g></g>
    <g id="blended"><rect width="20" height="20" fill="#20c0c0" style="mix-blend-mode: multiply"/></g>
    <g id="text"><text x="2" y="20" font-size="12">Ab</text></g>
  </svg>)svg";

/// A host drawing \p occurrences uses of \p fragment, ids `use-0` onwards, with no other content.
std::string MakeHost(int occurrences, std::string_view fragment, std::string_view useAttributes) {
  std::ostringstream out;
  out << R"(<svg xmlns="http://www.w3.org/2000/svg" width=")" << kHostWidth << "\" height=\""
      << kHostHeight << R"(" color="#30a0e0">)";
  for (int i = 0; i < occurrences; ++i) {
    out << "<use id=\"use-" << i << "\" href=\"" << kExternalResourceUrl << "#" << fragment << "\" "
        << useAttributes << "/>";
  }
  out << "</svg>";
  return out.str();
}

struct SpriteScene {
  ExternalUseDocument host;
  std::vector<SVGUseElement> uses;

  /// The sprite sheet, once a draw has loaded it.
  SVGDocument sheet() const { return SVGDocument::CreateFromHandle(*host.resource); }
};

std::optional<SpriteScene> MakeScene(int occurrences, std::string_view fragment = "sprite-0",
                                     std::string_view useAttributes = "") {
  std::optional<ExternalUseDocument> host =
      MakeExternalUseDocument(MakeHost(occurrences, fragment, useAttributes), std::string(kSheet));
  if (!host) {
    return std::nullopt;
  }
  SpriteScene scene{std::move(*host), {}};
  for (int i = 0; i < occurrences; ++i) {
    scene.uses.push_back(
        scene.host.document.querySelector("#use-" + std::to_string(i))->cast<SVGUseElement>());
  }
  return scene;
}

/// Places every occurrence for \p frame; \p step is the per-frame motion in pixels. Occurrences
/// stay clear of the canvas edges: one whose bounds reach past the canvas is drawn but not
/// recorded, since only its visible part was rasterized.
void PlaceAll(SpriteScene& scene, int frame, double step) {
  for (int i = 0; i < static_cast<int>(scene.uses.size()); ++i) {
    scene.uses[i].setTransform(Transform2d::Translate(
        Vector2d(24.0 + (i % 5) * 44.0 + frame * step, 24.0 + (i / 5) * 60.0 + frame * step)));
  }
}

/// Per-frame work a backend reported.
struct FrameWork {
  std::size_t spriteHits = 0;
  std::size_t spriteRecords = 0;
  std::size_t filterExecutions = 0;
  std::size_t geometryDraws = 0;
};

FrameWork Draw(RendererInterface& renderer, SpriteScene& scene) {
  renderer.draw(scene.host.document);
  const RendererResourceStats stats = renderer.resourceStats();
  return {stats.spriteCacheHits, stats.spriteCacheRecords, stats.filterExecutions,
          stats.geometryDraws};
}

/// Skips unless this binary links the Geode backend, which caches sprites, with filters.
#define REQUIRE_CACHING_BACKEND()                                                        \
  do {                                                                                   \
    if (!IsRendererBackendAvailable(RendererBackend::Geode) ||                           \
        !RendererBackendSupportsFeature(RendererBackend::Geode,                          \
                                        RendererBackendFeature::FilterEffects)) {        \
      GTEST_SKIP() << "Sprite caching is implemented by the Geode backend with filters"; \
    }                                                                                    \
  } while (false)

std::unique_ptr<RendererInterface> MakeCachingRenderer() {
  return CreateRendererInstance(RendererBackend::Geode);
}

/// Render the scene's current state with a fresh renderer whose caches are disabled.
RendererBitmap RenderUncached(SpriteScene& scene) {
  std::unique_ptr<RendererInterface> renderer = CreateRendererInstance(RendererBackend::Geode);
  renderer->setFilterResultCacheEnabled(false);
  renderer->draw(scene.host.document);
  return renderer->takeSnapshot();
}

void ExpectMatchesUncached(RendererInterface& renderer, SpriteScene& scene, std::string_view label,
                           const editor::tests::BitmapGoldenCompareParams& params =
                               editor::tests::PixelmatchIdentityParams()) {
  editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), RenderUncached(scene),
                                       std::string(label), params);
}

TEST(ExternalUseSpriteCacheTest, RasterizesOnceAcrossFramesAndOccurrences) {
  REQUIRE_CACHING_BACKEND();
  constexpr int kOccurrences = 6;
  constexpr int kFrames = 6;
  std::optional<SpriteScene> scene = MakeScene(kOccurrences);
  ASSERT_TRUE(scene.has_value());
  std::unique_ptr<RendererInterface> renderer = MakeCachingRenderer();

  FrameWork total;
  for (int frame = 0; frame < kFrames; ++frame) {
    PlaceAll(*scene, frame, 3.0);
    const FrameWork work = Draw(*renderer, *scene);
    total.spriteHits += work.spriteHits;
    total.spriteRecords += work.spriteRecords;
    if (frame > 0) {
      EXPECT_EQ(work.spriteHits, static_cast<std::size_t>(kOccurrences)) << "frame " << frame;
      EXPECT_EQ(work.geometryDraws, 0u) << "A cached occurrence draws no paths, frame " << frame;
      EXPECT_EQ(work.filterExecutions, 0u) << "A cached occurrence runs no filter, frame " << frame;
    }
  }
  // The first occurrence measures the sprite, the second notes the miss and the third records it
  // (two-touch admission); every later occurrence on every frame draws the rasterization.
  EXPECT_EQ(total.spriteRecords, 1u);
  EXPECT_EQ(total.spriteHits, static_cast<std::size_t>(kOccurrences * kFrames - 3));
  ExpectMatchesUncached(*renderer, *scene, "sprite-integer-motion");
}

TEST(ExternalUseSpriteCacheTest, DisabledCacheDrawsEveryOccurrence) {
  REQUIRE_CACHING_BACKEND();
  std::optional<SpriteScene> scene = MakeScene(3);
  ASSERT_TRUE(scene.has_value());
  std::unique_ptr<RendererInterface> renderer = MakeCachingRenderer();
  renderer->setSpriteCacheEnabled(false);
  for (int frame = 0; frame < 3; ++frame) {
    PlaceAll(*scene, frame, 1.0);
    const FrameWork work = Draw(*renderer, *scene);
    EXPECT_EQ(work.spriteHits, 0u);
    EXPECT_EQ(work.spriteRecords, 0u);
    EXPECT_GT(work.geometryDraws, 0u);
  }
}

// Translation and the occurrence's own opacity are applied outside the cached content, so
// changing them keeps every occurrence cached and matches an uncached render exactly.
TEST(ExternalUseSpriteCacheTest, TranslationAndOpacityKeepTheCache) {
  REQUIRE_CACHING_BACKEND();
  std::optional<SpriteScene> scene = MakeScene(3);
  ASSERT_TRUE(scene.has_value());
  std::unique_ptr<RendererInterface> renderer = MakeCachingRenderer();
  PlaceAll(*scene, 0, 1.0);
  (void)Draw(*renderer, *scene);
  (void)Draw(*renderer, *scene);
  for (int frame = 1; frame < 4; ++frame) {
    PlaceAll(*scene, frame, 5.0);
    scene->uses[1].setAttribute("opacity", std::to_string(0.9 - 0.2 * frame));
    const FrameWork work = Draw(*renderer, *scene);
    EXPECT_EQ(work.spriteRecords, 0u) << "frame " << frame;
    EXPECT_EQ(work.spriteHits, 3u) << "frame " << frame;
  }
  ExpectMatchesUncached(*renderer, *scene, "sprite-translation-opacity");
}

// Moving occurrences through their `x` and `y` attributes translates them like a transform does:
// after the first recording every frame draws them from the cache, exactly.
TEST(ExternalUseSpriteCacheTest, AttributeMotionKeepsTheCache) {
  REQUIRE_CACHING_BACKEND();
  std::optional<SpriteScene> scene = MakeScene(3);
  ASSERT_TRUE(scene.has_value());
  std::unique_ptr<RendererInterface> renderer = MakeCachingRenderer();
  std::size_t records = 0;
  for (int frame = 0; frame < 8; ++frame) {
    for (int i = 0; i < 3; ++i) {
      scene->uses[i].setAttribute("x", std::to_string(24 + 60 * i + 3 * frame));
      scene->uses[i].setAttribute("y", std::to_string(24 + 2 * frame));
    }
    const FrameWork work = Draw(*renderer, *scene);
    records += work.spriteRecords;
    if (frame >= 2) {
      EXPECT_EQ(work.spriteHits, 3u) << "frame " << frame;
    }
  }
  EXPECT_EQ(records, 1u);
  ExpectMatchesUncached(*renderer, *scene, "sprite-attribute-motion");
}

/**
 * A rasterization kept per quarter-pixel bucket is drawn on the pixel grid: a hit at a fractional
 * translation in the recorded bucket is exactly the uncached render at the recorded translation
 * moved by whole pixels, and differs from the render at the actual translation only by the
 * sub-quarter-pixel quantization of edges.
 */
TEST(ExternalUseSpriteCacheTest, FractionalMotionStaysInItsBucket) {
  REQUIRE_CACHING_BACKEND();
  std::optional<SpriteScene> scene = MakeScene(1);
  ASSERT_TRUE(scene.has_value());
  const Vector2d recorded(10.1, 20.1);
  const Vector2d moved(13.15, 22.2);  // Same quarter-pixel bucket, (3, 2) whole pixels away.
  std::unique_ptr<RendererInterface> renderer = MakeCachingRenderer();
  scene->uses[0].setTransform(Transform2d::Translate(recorded));
  for (int frame = 0; frame < 3; ++frame) {
    (void)Draw(*renderer, *scene);
  }
  ASSERT_EQ(Draw(*renderer, *scene).spriteHits, 1u);
  editor::tests::CompareBitmapToBitmap(renderer->takeSnapshot(), RenderUncached(*scene),
                                       "sprite-at-recorded-translation",
                                       editor::tests::PixelmatchIdentityParams());
  scene->uses[0].setTransform(Transform2d::Translate(moved));
  ASSERT_EQ(Draw(*renderer, *scene).spriteHits, 1u);
  const RendererBitmap cached = renderer->takeSnapshot();

  scene->uses[0].setTransform(Transform2d::Translate(recorded + Vector2d(3.0, 2.0)));
  editor::tests::CompareBitmapToBitmap(cached, RenderUncached(*scene), "sprite-recorded-placement",
                                       editor::tests::PixelmatchIdentityParams());
  scene->uses[0].setTransform(Transform2d::Translate(moved));
  editor::tests::CompareBitmapToBitmap(cached, RenderUncached(*scene), "sprite-quantization",
                                       editor::tests::ApprovedPixelToleranceParams(0.1f, 160));
}

// A sprite that paints only one blurred element whose blur fades inside its region is resampled
// at any sub-pixel offset: one rasterization serves every fractional position, within the
// documented bilinear error of a low-passed result.
TEST(ExternalUseSpriteCacheTest, BlurredSpriteIsResampledAtAnyOffset) {
  REQUIRE_CACHING_BACKEND();
  std::optional<SpriteScene> scene = MakeScene(3, "blurred");
  ASSERT_TRUE(scene.has_value());
  std::unique_ptr<RendererInterface> renderer = MakeCachingRenderer();
  std::size_t records = 0;
  for (int frame = 0; frame < 8; ++frame) {
    PlaceAll(*scene, frame, 1.37);
    const FrameWork work = Draw(*renderer, *scene);
    records += work.spriteRecords;
    if (frame > 0) {
      EXPECT_EQ(work.spriteHits, 3u) << "frame " << frame;
    }
  }
  EXPECT_EQ(records, 1u) << "One rasterization serves every fractional offset.";
  // Bilinear resampling of a result already low-passed by a blur of several pixels moves each
  // premultiplied channel by at most a few levels (4 measured here).
  ExpectMatchesUncached(*renderer, *scene, "sprite-resampled-blur",
                        editor::tests::ApprovedPixelToleranceParams(0.03f, 0));
}

/// After warm frames, \p mutate changes what the occurrence renders: the next frame must record
/// again and match an uncached render exactly.
void ExpectMutationRerasterizes(SpriteScene& scene, std::string_view label,
                                const std::function<void(SpriteScene&)>& mutate) {
  std::unique_ptr<RendererInterface> renderer = MakeCachingRenderer();
  PlaceAll(scene, 0, 1.0);
  std::size_t warmHits = 0;
  for (int frame = 0; frame < 3; ++frame) {
    warmHits += Draw(*renderer, scene).spriteHits;
  }
  EXPECT_GT(warmHits, 0u) << label << ": the warm frames must draw cached sprites";
  const RendererBitmap before = RenderUncached(scene);
  mutate(scene);
  for (int frame = 0; frame < 3; ++frame) {
    (void)Draw(*renderer, scene);
  }
  const RendererBitmap after = RenderUncached(scene);
  int changedPixels = -1;
  if (after.dimensions == before.dimensions) {
    editor::tests::CompareBitmapToBitmap(after, before, std::string(label) + "-mutation-visible",
                                         editor::tests::ApprovedPixelToleranceParams(0.0f, 1 << 30),
                                         &changedPixels);
  }
  EXPECT_NE(changedPixels, 0) << label << ": the mutation must change the rendering";
  editor::tests::CompareBitmapToBitmap(renderer->takeSnapshot(), after, std::string(label),
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(ExternalUseSpriteCacheTest, SheetMutationRerasterizes) {
  REQUIRE_CACHING_BACKEND();
  std::optional<SpriteScene> scene = MakeScene(3);
  ASSERT_TRUE(scene.has_value());
  ExpectMutationRerasterizes(*scene, "sheet-mutation", [](SpriteScene& s) {
    s.sheet().querySelector("#body-0")->setAttribute("width", "14");
  });
}

TEST(ExternalUseSpriteCacheTest, SheetFilterChangeRerasterizes) {
  REQUIRE_CACHING_BACKEND();
  std::optional<SpriteScene> scene = MakeScene(3);
  ASSERT_TRUE(scene.has_value());
  ExpectMutationRerasterizes(*scene, "sheet-filter", [](SpriteScene& s) {
    s.sheet().querySelector("#blur-node")->setAttribute("stdDeviation", "3");
  });
}

TEST(ExternalUseSpriteCacheTest, SheetGradientChangeRerasterizes) {
  REQUIRE_CACHING_BACKEND();
  std::optional<SpriteScene> scene = MakeScene(3);
  ASSERT_TRUE(scene.has_value());
  ExpectMutationRerasterizes(*scene, "sheet-gradient", [](SpriteScene& s) {
    s.sheet().querySelector("stop")->setAttribute("stop-color", "#10d010");
  });
}

TEST(ExternalUseSpriteCacheTest, InheritedHostStyleRerasterizes) {
  REQUIRE_CACHING_BACKEND();
  std::optional<SpriteScene> scene = MakeScene(3);
  ASSERT_TRUE(scene.has_value());
  ExpectMutationRerasterizes(*scene, "inherited-color",
                             [](SpriteScene& s) { s.uses[1].setAttribute("color", "#e0a000"); });
}

TEST(ExternalUseSpriteCacheTest, RetargetingTheUseRerasterizes) {
  REQUIRE_CACHING_BACKEND();
  std::optional<SpriteScene> scene = MakeScene(3);
  ASSERT_TRUE(scene.has_value());
  ExpectMutationRerasterizes(*scene, "retargeted", [](SpriteScene& s) {
    s.uses[0].setHref(RcString(std::string(kExternalResourceUrl) + "#sprite-1"));
  });
}

// The viewport a `<use>` establishes for a `<symbol>` is part of the sprite's identity: uses of
// one symbol at two sizes keep one rasterization each. (Changing a use's size at runtime does not
// yet re-lay out the instance, uncached or not, so sizes are set up front.)
TEST(ExternalUseSpriteCacheTest, SymbolUsesOfDifferentSizesKeepSeparateSprites) {
  REQUIRE_CACHING_BACKEND();
  std::optional<ExternalUseDocument> host = MakeExternalUseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="256" height="192">
        <use href="sprites.svg#symbol" width="32" height="32" transform="translate(10 10)"/>
        <use href="sprites.svg#symbol" width="20" height="20" transform="translate(60 10)"/>
        <use href="sprites.svg#symbol" width="32" height="32" transform="translate(110 10)"/>
        <use href="sprites.svg#symbol" width="20" height="20" transform="translate(160 10)"/>
      </svg>)svg",
      std::string(kSheet));
  ASSERT_TRUE(host.has_value());
  SpriteScene scene{std::move(*host), {}};
  std::unique_ptr<RendererInterface> renderer = MakeCachingRenderer();
  std::size_t records = 0;
  for (int frame = 0; frame < 4; ++frame) {
    const FrameWork work = Draw(*renderer, scene);
    records += work.spriteRecords;
    if (frame >= 2) {
      EXPECT_EQ(work.spriteHits, 4u) << "frame " << frame;
    }
  }
  EXPECT_EQ(records, 2u) << "One rasterization per symbol size.";
  ExpectMatchesUncached(*renderer, scene, "symbol-sizes");
}

TEST(ExternalUseSpriteCacheTest, ScalingAnOccurrenceRerasterizes) {
  REQUIRE_CACHING_BACKEND();
  std::optional<SpriteScene> scene = MakeScene(3);
  ASSERT_TRUE(scene.has_value());
  ExpectMutationRerasterizes(*scene, "scaled", [](SpriteScene& s) {
    s.uses[1].setTransform(Transform2d::Scale(1.5) * Transform2d::Translate(Vector2d(90, 60)));
  });
}

TEST(ExternalUseSpriteCacheTest, ChangingTheDeviceScaleRerasterizes) {
  REQUIRE_CACHING_BACKEND();
  std::optional<SpriteScene> scene = MakeScene(2);
  ASSERT_TRUE(scene.has_value());
  ExpectMutationRerasterizes(*scene, "device-scale", [](SpriteScene& s) {
    s.host.document.setCanvasSize(kHostWidth * 2, kHostHeight * 2);
  });
}

// Several static scales each keep their own rasterization; a scale that changes every frame is
// never admitted, so it draws uncached without recording.
TEST(ExternalUseSpriteCacheTest, StaticScalesCacheAndAnimatedScaleDoesNotThrash) {
  REQUIRE_CACHING_BACKEND();
  std::optional<SpriteScene> scene = MakeScene(4);
  ASSERT_TRUE(scene.has_value());
  std::unique_ptr<RendererInterface> renderer = MakeCachingRenderer();
  std::size_t records = 0;
  for (int frame = 0; frame < 8; ++frame) {
    for (int i = 0; i < 3; ++i) {
      scene->uses[i].setTransform(Transform2d::Scale(1.0 + 0.25 * i) *
                                  Transform2d::Translate(Vector2d(10.0 + 60.0 * i, 20.0)));
    }
    scene->uses[3].setTransform(Transform2d::Scale(1.0 + 0.03 * (frame + 1)) *
                                Transform2d::Translate(Vector2d(40.0, 120.0)));
    const FrameWork work = Draw(*renderer, *scene);
    records += work.spriteRecords;
    if (frame >= 3) {
      EXPECT_EQ(work.spriteHits, 3u) << "frame " << frame;
      EXPECT_EQ(work.spriteRecords, 0u) << "frame " << frame;
    }
  }
  EXPECT_EQ(records, 3u) << "One rasterization per static scale.";
  ExpectMatchesUncached(*renderer, *scene, "sprite-scales");
}

/// \p fragment is never cached, and draws exactly like an uncached render.
void ExpectNeverCached(std::string_view fragment, std::string_view useAttributes,
                       std::string_view label) {
  std::optional<SpriteScene> scene = MakeScene(3, fragment, useAttributes);
  ASSERT_TRUE(scene.has_value());
  std::unique_ptr<RendererInterface> renderer = MakeCachingRenderer();
  PlaceAll(*scene, 0, 1.0);
  for (int frame = 0; frame < 3; ++frame) {
    const FrameWork work = Draw(*renderer, *scene);
    EXPECT_EQ(work.spriteHits, 0u) << label;
    EXPECT_EQ(work.spriteRecords, 0u) << label;
  }
  ExpectMatchesUncached(*renderer, *scene, label);
}

TEST(ExternalUseSpriteCacheTest, ImageIsNotCached) {
  REQUIRE_CACHING_BACKEND();
  ExpectNeverCached("imaged", "", "sprite-image");
}

// `opacity: inherit` on the referenced root reads the `<use>` element's own opacity, which the
// occurrence inputs leave out, so such a sprite is never cached.
TEST(ExternalUseSpriteCacheTest, InheritOfANonInheritedPropertyIsNotCached) {
  REQUIRE_CACHING_BACKEND();
  std::optional<SpriteScene> scene = MakeScene(3, "inheritor", R"(opacity="0.5")");
  ASSERT_TRUE(scene.has_value());
  std::unique_ptr<RendererInterface> renderer = MakeCachingRenderer();
  PlaceAll(*scene, 0, 1.0);
  for (int frame = 0; frame < 3; ++frame) {
    EXPECT_EQ(Draw(*renderer, *scene).spriteHits, 0u);
  }
  ExpectMatchesUncached(*renderer, *scene, "sprite-inherit-opacity");
  scene->uses[1].setAttribute("opacity", "0.9");
  (void)Draw(*renderer, *scene);
  ExpectMatchesUncached(*renderer, *scene, "sprite-inherit-opacity-changed");
}

/// Draws \p host over the test sheet for a few frames with a caching renderer and requires the
/// last frame to match an uncached render exactly; returns the sprite hits of the last frame.
std::size_t ExpectHostMatchesUncached(std::string_view host, std::string_view label) {
  std::optional<ExternalUseDocument> document = MakeExternalUseDocument(host, std::string(kSheet));
  EXPECT_TRUE(document.has_value());
  if (!document) {
    return 0;
  }
  SpriteScene scene{std::move(*document), {}};
  std::unique_ptr<RendererInterface> renderer = MakeCachingRenderer();
  std::size_t hits = 0;
  for (int frame = 0; frame < 4; ++frame) {
    hits = Draw(*renderer, scene).spriteHits;
  }
  ExpectMatchesUncached(*renderer, scene, label);
  return hits;
}

// A size the `<use>` gets from CSS is part of what the occurrence renders: uses of one symbol
// sized by attributes and by a stylesheet each match an uncached render.
TEST(ExternalUseSpriteCacheTest, CssSizeOnTheUseMatchesUncached) {
  REQUIRE_CACHING_BACKEND();
  (void)ExpectHostMatchesUncached(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="256" height="192">
        <style>.big { width: 48px; height: 48px }</style>
        <use href="sprites.svg#symbol" width="32" height="32" transform="translate(10 10)"/>
        <use class="big" href="sprites.svg#symbol" width="32" height="32"
             transform="translate(60 10)"/>
        <use href="sprites.svg#symbol" width="32" height="32" transform="translate(120 10)"/>
        <use class="big" href="sprites.svg#symbol" width="32" height="32"
             transform="translate(170 10)"/>
      </svg>)svg",
      "sprite-css-size");
}

// A size in viewport units resolves against the host's viewport, so resizing the host canvas
// rasterizes again.
TEST(ExternalUseSpriteCacheTest, ViewportUnitSizeFollowsTheHostViewport) {
  REQUIRE_CACHING_BACKEND();
  std::optional<SpriteScene> scene = MakeScene(3, "symbol", R"(width="10vw" height="10vw")");
  ASSERT_TRUE(scene.has_value());
  ExpectMutationRerasterizes(*scene, "sprite-vw-resize", [](SpriteScene& s) {
    s.host.document.setCanvasSize(kHostWidth * 3 / 2, kHostHeight * 3 / 2);
  });
}

// A clip around the `<use>` in the host applies to the cached sprite as it does to its content.
TEST(ExternalUseSpriteCacheTest, HostClipAroundTheUseMatchesUncached) {
  REQUIRE_CACHING_BACKEND();
  EXPECT_GT(ExpectHostMatchesUncached(
                R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="256" height="192">
        <clipPath id="hostclip"><circle cx="100" cy="40" r="60"/></clipPath>
        <g clip-path="url(#hostclip)">
          <use href="sprites.svg#sprite-0" transform="translate(40 20)"/>
          <use href="sprites.svg#sprite-0" transform="translate(80 20)"/>
          <use href="sprites.svg#sprite-0" transform="translate(120 20)"/>
        </g>
      </svg>)svg",
                "sprite-host-clip"),
            0u);
}

// Inside a host filter the occurrences draw uncached, through the host filter.
TEST(ExternalUseSpriteCacheTest, HostFilterAroundTheUseDrawsUncached) {
  REQUIRE_CACHING_BACKEND();
  EXPECT_EQ(ExpectHostMatchesUncached(
                R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="256" height="192">
        <filter id="hostblur"><feGaussianBlur stdDeviation="1"/></filter>
        <g filter="url(#hostblur)">
          <use href="sprites.svg#sprite-0" transform="translate(40 20)"/>
          <use href="sprites.svg#sprite-0" transform="translate(80 20)"/>
          <use href="sprites.svg#sprite-0" transform="translate(120 20)"/>
        </g>
      </svg>)svg",
                "sprite-host-filter"),
            0u);
}

// A `<symbol>` whose viewBox scales its content, with a blurred child, moving by fractions of a
// pixel: within the bucket quantization bound of an uncached render.
TEST(ExternalUseSpriteCacheTest, ScaledSymbolWithBlurAtFractionalOffsets) {
  REQUIRE_CACHING_BACKEND();
  std::optional<SpriteScene> scene = MakeScene(3, "blursym", R"(width="32" height="32")");
  ASSERT_TRUE(scene.has_value());
  std::unique_ptr<RendererInterface> renderer = MakeCachingRenderer();
  std::size_t hits = 0;
  for (int frame = 0; frame < 8; ++frame) {
    PlaceAll(*scene, frame, 1.37);
    hits += Draw(*renderer, *scene).spriteHits;
  }
  EXPECT_GT(hits, 0u);
  ExpectMatchesUncached(*renderer, *scene, "sprite-scaled-symbol-blur",
                        editor::tests::ApprovedPixelToleranceParams(0.1f, 160));
}

// Lighting from a distant light moves with its input, so a sprite lit that way is cached, and
// matches an uncached render exactly; a point light is positioned on the raster, so it is not.
TEST(ExternalUseSpriteCacheTest, DistantLightIsCachedAndPointLightIsNot) {
  REQUIRE_CACHING_BACKEND();
  std::optional<SpriteScene> scene = MakeScene(3, "lit");
  ASSERT_TRUE(scene.has_value());
  std::unique_ptr<RendererInterface> renderer = MakeCachingRenderer();
  std::size_t hits = 0;
  for (int frame = 0; frame < 4; ++frame) {
    PlaceAll(*scene, frame, 2.0);
    hits += Draw(*renderer, *scene).spriteHits;
  }
  EXPECT_GT(hits, 0u);
  ExpectMatchesUncached(*renderer, *scene, "sprite-distant-light");
  ExpectNeverCached("pointlit", "", "sprite-point-light");
}

// Sprites recorded in isolation record whole even when they reach past the canvas: large sprites
// partly off the canvas, drawn before the rest, record once and then hit like every other sprite,
// so they cannot starve the later sprites' recordings.
TEST(ExternalUseSpriteCacheTest, OffCanvasSpritesRecordAndDoNotStarveOthers) {
  REQUIRE_CACHING_BACKEND();
  std::ostringstream host;
  host << R"(<svg xmlns="http://www.w3.org/2000/svg" width="256" height="192">)";
  for (int i = 0; i < 12; ++i) {
    host << "<use id=\"use-" << i << "\" href=\"sprites.svg#sprite-0\"/>";
  }
  for (int i = 12; i < 32; ++i) {
    host << "<use id=\"use-" << i << "\" href=\"sprites.svg#sprite-1\"/>";
  }
  host << "</svg>";
  std::optional<ExternalUseDocument> document = MakeExternalUseDocument(host.str(),
                                                                        std::string(kSheet));
  ASSERT_TRUE(document.has_value());
  SpriteScene scene{std::move(*document), {}};
  for (int i = 0; i < 32; ++i) {
    scene.uses.push_back(
        scene.host.document.querySelector("#use-" + std::to_string(i))->cast<SVGUseElement>());
  }
  std::unique_ptr<RendererInterface> renderer = MakeCachingRenderer();
  for (int frame = 0; frame < 6; ++frame) {
    for (int i = 0; i < 12; ++i) {
      // Doubled and hanging past the left, top or bottom edge.
      scene.uses[i].setTransform(Transform2d::Scale(2.0) *
                                 Transform2d::Translate(Vector2d(-24.0 + (i % 4) * 70.0 + frame,
                                                                 i < 4 ? -30.0 : 160.0)));
    }
    for (int i = 12; i < 32; ++i) {
      scene.uses[i].setTransform(Transform2d::Translate(
          Vector2d(10.0 + ((i - 12) % 10) * 24.0 + frame, 60.0 + ((i - 12) / 10) * 40.0)));
    }
    const FrameWork work = Draw(*renderer, scene);
    if (frame >= 3) {
      EXPECT_EQ(work.spriteHits, 32u) << "frame " << frame;
      EXPECT_EQ(work.spriteRecords, 0u) << "frame " << frame;
    }
  }
  ExpectMatchesUncached(*renderer, scene, "sprite-off-canvas");
}

/// Largest premultiplied channel difference, and the number of pixels differing by more than
/// \p levels, between two bitmaps of one size.
std::pair<int, int> PremultipliedDifference(const RendererBitmap& a, const RendererBitmap& b,
                                            int levels) {
  int maximum = 0;
  int count = 0;
  for (int y = 0; y < a.dimensions.y; ++y) {
    for (int x = 0; x < a.dimensions.x; ++x) {
      const std::uint8_t* pa = &a.pixels[static_cast<std::size_t>(y) * a.rowBytes + x * 4u];
      const std::uint8_t* pb = &b.pixels[static_cast<std::size_t>(y) * b.rowBytes + x * 4u];
      int pixel = 0;
      for (int c = 0; c < 4; ++c) {
        const int va = c == 3 ? pa[3] : pa[c] * pa[3] / 255;
        const int vb = c == 3 ? pb[3] : pb[c] * pb[3] / 255;
        pixel = std::max(pixel, std::abs(va - vb));
      }
      maximum = std::max(maximum, pixel);
      count += pixel > levels ? 1 : 0;
    }
  }
  return {maximum, count};
}

// Under the resample policy a sprite moving by fractions of a pixel every frame hits on every
// frame after its one recording, and differs from an exact render only at its edges, by at most
// half a pixel's coverage.
TEST(ExternalUseSpriteCacheTest, ResamplePolicyHitsUnderSubpixelMotion) {
  REQUIRE_CACHING_BACKEND();
  std::optional<SpriteScene> scene = MakeScene(3);
  ASSERT_TRUE(scene.has_value());
  std::unique_ptr<RendererInterface> renderer = MakeCachingRenderer();
  renderer->setSpriteSubpixelPlacement(SpriteSubpixelPlacement::Resample);
  std::size_t records = 0;
  for (int frame = 0; frame < 10; ++frame) {
    PlaceAll(*scene, frame, 0.37);
    const FrameWork work = Draw(*renderer, *scene);
    records += work.spriteRecords;
    if (frame >= 2) {
      EXPECT_EQ(work.spriteHits, 3u) << "frame " << frame;
    }
  }
  EXPECT_EQ(records, 1u);
  const RendererBitmap cached = renderer->takeSnapshot();
  const RendererBitmap exact = RenderUncached(*scene);
  ASSERT_EQ(cached.dimensions, exact.dimensions);
  const auto [maximum, edgePixels] = PremultipliedDifference(cached, exact, 2);
  EXPECT_LE(maximum, 160) << "Resampling moves an edge by at most half a pixel of coverage";
  // Each 32x32 sprite has at most a few hundred edge pixels; its interior matches.
  EXPECT_LE(edgePixels, 3 * 400);
}

TEST(ExternalUseSpriteCacheTest, BackdropBlendIsNotCached) {
  REQUIRE_CACHING_BACKEND();
  ExpectNeverCached("blended", "", "sprite-blend");
}

TEST(ExternalUseSpriteCacheTest, TextIsNotCached) {
  REQUIRE_CACHING_BACKEND();
  ExpectNeverCached("text", "", "sprite-text");
}

TEST(ExternalUseSpriteCacheTest, HostGradientPaintIsNotCached) {
  REQUIRE_CACHING_BACKEND();
  std::optional<ExternalUseDocument> host = MakeExternalUseDocument(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="256" height="192">
        <defs><linearGradient id="hostgrad"><stop offset="0" stop-color="red"/>
          <stop offset="1" stop-color="blue"/></linearGradient></defs>
        <use href="sprites.svg#sprite-1" fill="url(#hostgrad)" transform="translate(10 10)"/>
        <use href="sprites.svg#sprite-1" fill="url(#hostgrad)" transform="translate(60 10)"/>
        <use href="sprites.svg#sprite-1" fill="url(#hostgrad)" transform="translate(110 10)"/>
      </svg>)svg",
      std::string(kSheet));
  ASSERT_TRUE(host.has_value());
  SpriteScene scene{std::move(*host), {}};
  std::unique_ptr<RendererInterface> renderer = MakeCachingRenderer();
  for (int frame = 0; frame < 3; ++frame) {
    EXPECT_EQ(Draw(*renderer, scene).spriteHits, 0u);
  }
  ExpectMatchesUncached(*renderer, scene, "sprite-host-gradient");
}

// Disabling the filter result cache drops held sprites, which share its memory.
TEST(ExternalUseSpriteCacheTest, DisablingTheFilterCacheDropsSprites) {
  REQUIRE_CACHING_BACKEND();
  std::optional<SpriteScene> scene = MakeScene(3);
  ASSERT_TRUE(scene.has_value());
  std::unique_ptr<RendererInterface> renderer = MakeCachingRenderer();
  PlaceAll(*scene, 0, 1.0);
  (void)Draw(*renderer, *scene);
  (void)Draw(*renderer, *scene);
  ASSERT_GT(renderer->resourceStats().filterResultCacheEntries, 0u);
  renderer->setFilterResultCacheEnabled(false);
  EXPECT_EQ(renderer->resourceStats().filterResultCacheEntries, 0u);
  EXPECT_EQ(Draw(*renderer, *scene).spriteHits, 0u);
}

}  // namespace
}  // namespace donner::svg::tests
