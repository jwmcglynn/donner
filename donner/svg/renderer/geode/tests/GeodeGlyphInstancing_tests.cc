/// @file
/// End-to-end gate for glyph-outline residency and per-occurrence instancing.
///
/// The unit of work these tests are about is the glyph OCCURRENCE: a letter at
/// one position in one run. Text used to pay for every occurrence in full - the
/// outline was fetched from the font backend, transformed into place, banded,
/// and uploaded, once per occurrence per frame. The behaviour under test is
/// that the expensive half of that is now paid once per unique glyph outline
/// and retained, while the occurrence contributes only an instance record.
///
/// The observables are the frame counters (`glyphResidencyUploads`,
/// `glyphResidencyHits`, `glyphResidencyEvictions`, `pathEncodes`,
/// `drawCalls`) plus pixels, because a counter that improves while the text
/// stops rendering is not an improvement.
///
/// Fonts come from the checked-in hermetic set: glyph identity is the cache
/// key, so resolving fonts from the host would make every count here a
/// property of the machine that ran the test.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/Vector2.h"
#include "donner/base/tests/Runfiles.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/geode/GeodeCounters.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/tests/ImageComparisonTestFixture.h"

namespace donner::svg {
namespace {

/// Hermetic test fonts, relative to the runfiles root.
constexpr std::string_view kFontsRunfilesPath = "third_party/resvg-test-suite/fonts";

/// Pixels with a non-zero alpha. Used as the liveness signal: every counter
/// assertion here is paired with one so a "fix" that stops drawing the text
/// cannot pass.
size_t nonTransparentPixels(const RendererBitmap& bitmap) {
  size_t count = 0;
  for (int y = 0; y < bitmap.dimensions.y; ++y) {
    const uint8_t* row = bitmap.pixels.data() + static_cast<size_t>(y) * bitmap.rowBytes;
    for (int x = 0; x < bitmap.dimensions.x; ++x) {
      if (row[x * 4 + 3] != 0) {
        ++count;
      }
    }
  }
  return count;
}

/// Straight-alpha RGBA at (x, y).
struct Pixel {
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
  uint8_t a = 0;
};

Pixel pixelAt(const RendererBitmap& bitmap, int x, int y) {
  const size_t offset = static_cast<size_t>(y) * bitmap.rowBytes + static_cast<size_t>(x) * 4u;
  if (offset + 4 > bitmap.pixels.size()) {
    return Pixel{};
  }
  return Pixel{bitmap.pixels[offset], bitmap.pixels[offset + 1], bitmap.pixels[offset + 2],
               bitmap.pixels[offset + 3]};
}

class GeodeGlyphInstancingTest : public ::testing::Test {
protected:
  /// Process-wide shared device, matching the other GPU-opening geode suites:
  /// device creation dominates the runtime of a short test.
  static std::shared_ptr<geode::GeodeDevice> sharedDevice() {
    static auto device = [] {
      return std::shared_ptr<geode::GeodeDevice>(geode::GeodeDevice::CreateHeadless());
    }();
    return device;
  }

  void SetUp() override {
    ASSERT_TRUE(sharedDevice()) << "No headless GPU device; the glyph residency gate needs one.";
    const std::filesystem::path fontsDir =
        Runfiles::instance().Rlocation(std::string(kFontsRunfilesPath));
    ASSERT_TRUE(std::filesystem::is_directory(fontsDir))
        << "Hermetic test fonts not found at '" << fontsDir
        << "'; without them glyph identity depends on the host's installed fonts.";
  }

  /// Parse `source` and point its generic font families at the hermetic set.
  SVGDocument parse(std::string_view source, int canvasEdge = 200) {
    ParseWarningSink sink = ParseWarningSink::Disabled();
    auto parsed = parser::SVGParser::ParseSVG(source, sink);
    EXPECT_FALSE(parsed.hasError()) << (parsed.hasError() ? parsed.error().reason : "");
    SVGDocument document = std::move(parsed.result());
    TrustDocumentFontFacesForTesting(document);
    RegisterFontsFromDirectoryForTesting(
        document, Runfiles::instance().Rlocation(std::string(kFontsRunfilesPath)));
    document.setCanvasSize(canvasEdge, canvasEdge);
    return document;
  }

  /// One rendered frame plus the counters it cost.
  struct Frame {
    RendererBitmap bitmap;
    geode::GeodeCounters counters;
  };

  Frame render(RendererGeode& renderer, SVGDocument& document) {
    Frame frame;
    renderer.draw(document);
    frame.counters = renderer.lastFrameTimings().counters;
    frame.bitmap = renderer.takeSnapshot();
    return frame;
  }
};

/// A run repeats a handful of outlines across many occurrences. The first frame
/// pays one upload per DISTINCT outline; every later occurrence, in that frame
/// and in every frame after it, hits the resident entry and re-encodes nothing.
TEST_F(GeodeGlyphInstancingTest, RepeatedGlyphsShareOneResidentOutline) {
  // "eeee" is four occurrences of one outline, so the occurrence count and the
  // distinct-outline count cannot be confused for each other.
  SVGDocument document = parse(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200"
           font-family="Noto Sans" font-size="32">
        <text x="10" y="60" fill="black">eeee</text>
      </svg>)svg");

  RendererGeode renderer(sharedDevice());
  const Frame first = render(renderer, document);
  ASSERT_GT(nonTransparentPixels(first.bitmap), 0u) << "Text did not render at all.";

  EXPECT_EQ(first.counters.glyphResidencyUploads, 1u)
      << "Four occurrences of one outline must upload exactly one resident glyph.";
  EXPECT_EQ(first.counters.glyphResidencyHits, 3u)
      << "The second and later occurrences must come from the resident entry.";
  EXPECT_EQ(renderer.residentGlyphCountForTesting(document), 1u);

  const Frame second = render(renderer, document);
  EXPECT_EQ(second.counters.glyphResidencyUploads, 0u)
      << "An unchanged frame must not re-derive any glyph outline.";
  EXPECT_EQ(second.counters.glyphResidencyHits, 4u)
      << "Every occurrence of an unchanged frame comes from residency.";
  EXPECT_EQ(second.counters.pathEncodes, 0u) << "An unchanged text frame must re-encode nothing.";
  EXPECT_EQ(nonTransparentPixels(second.bitmap), nonTransparentPixels(first.bitmap))
      << "The resident second frame must draw the same coverage as the first.";
}

/// Two separate `<text>` elements are distinct source entities that share one
/// glyph outline: residency is keyed on the glyph, not on the element, so the
/// outline is built once for both.
///
/// Their DRAWS do not currently share a batch - each element's fill loop closes
/// its batch before anything else can emit, so batching collapses occurrences
/// within an element and cross-element batching is future work. What this pins
/// is the property batching can get wrong wherever it does apply: instances of
/// one draw must still composite in painter order.
TEST_F(GeodeGlyphInstancingTest, OverlappingTextElementsCompositeInPaintOrder) {
  // Both elements draw the same glyphs at the same place: opaque blue is
  // painted last, so blue must win everywhere the two overlap.
  SVGDocument document = parse(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200"
           font-family="Noto Sans" font-size="96">
        <text x="10" y="120" fill="#ff0000">HH</text>
        <text x="10" y="120" fill="#0000ff">HH</text>
      </svg>)svg");

  RendererGeode renderer(sharedDevice());
  const Frame first = render(renderer, document);
  const size_t covered = nonTransparentPixels(first.bitmap);
  ASSERT_GT(covered, 0u) << "Text did not render at all.";

  // One outline serves all four occurrences across the two elements.
  EXPECT_EQ(first.counters.glyphResidencyUploads, 1u);
  EXPECT_EQ(renderer.residentGlyphCountForTesting(document), 1u);

  // Find a pixel the glyphs fully cover and check the painter-order winner.
  bool foundOpaque = false;
  for (int y = 0; y < first.bitmap.dimensions.y && !foundOpaque; ++y) {
    for (int x = 0; x < first.bitmap.dimensions.x; ++x) {
      const Pixel pixel = pixelAt(first.bitmap, x, y);
      if (pixel.a == 255) {
        EXPECT_LT(pixel.r, 16) << "Red shows through at (" << x << ", " << y
                               << "); the later element must win in painter order.";
        EXPECT_GT(pixel.b, 200) << "Blue is missing at (" << x << ", " << y << ").";
        foundOpaque = true;
        break;
      }
    }
  }
  EXPECT_TRUE(foundOpaque) << "No fully covered glyph pixel to check painter order against.";

  const Frame second = render(renderer, document);
  EXPECT_EQ(second.counters.glyphResidencyUploads, 0u);
  EXPECT_EQ(second.counters.pathEncodes, 0u)
      << "Two elements' worth of glyphs must all come from residency on an unchanged frame.";
  EXPECT_EQ(nonTransparentPixels(second.bitmap), covered);

  if (RendererGeode::sceneBatchingEnabledForTesting()) {
    EXPECT_LT(second.counters.drawCalls, 4u)
        << "Four glyph occurrences over one resident outline must collapse into fewer draws "
           "than occurrences.";
  }
}

/// Residency is budgeted. Under a budget smaller than the working set the
/// oldest unused entries are dropped, misses that cannot be admitted use
/// frame-scoped geometry, and the text keeps rendering identically throughout.
TEST_F(GeodeGlyphInstancingTest, EvictionUnderPressureKeepsRenderingCorrect) {
  SVGDocument document = parse(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200"
           font-family="Noto Sans" font-size="24">
        <text x="10" y="60" fill="black">abcdefghij</text>
      </svg>)svg");

  RendererGeode renderer(sharedDevice());
  const Frame unbudgeted = render(renderer, document);
  const size_t covered = nonTransparentPixels(unbudgeted.bitmap);
  ASSERT_GT(covered, 0u) << "Text did not render at all.";
  ASSERT_GT(renderer.residentGlyphCountForTesting(document), 2u)
      << "This document needs more distinct glyphs than the budget below to create pressure.";
  EXPECT_EQ(unbudgeted.counters.glyphResidencyEvictions, 0u)
      << "The default budget must not evict a ten-glyph document.";

  renderer.setGlyphResidencyBudgetForTesting(/*maxEntries=*/2, /*maxEncodedBytes=*/1u << 30);

  const Frame squeezed = render(renderer, document);
  EXPECT_GT(squeezed.counters.glyphResidencyEvictions, 0u)
      << "A budget below the working set must drop entries.";
  EXPECT_LE(renderer.residentGlyphCountForTesting(document), 2u)
      << "The current frame must not repopulate the cache past its admission limit.";
  EXPECT_EQ(nonTransparentPixels(squeezed.bitmap), covered)
      << "Eviction must not change what the frame draws.";

  // Still correct once the budget is lifted again: the entries that survived
  // the squeeze are still usable, not left half-released.
  renderer.setGlyphResidencyBudgetForTesting(/*maxEntries=*/1024, /*maxEncodedBytes=*/1u << 30);
  const Frame restored = render(renderer, document);
  EXPECT_EQ(nonTransparentPixels(restored.bitmap), covered);
}

TEST_F(GeodeGlyphInstancingTest, FirstFrameAdmissionHonorsResidencyEntryBudget) {
  SVGDocument document = parse(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200"
           font-family="Noto Sans" font-size="24">
        <text x="10" y="60" fill="black">abcdefghij</text>
      </svg>)svg");

  RendererGeode renderer(sharedDevice());
  constexpr size_t kMaximumEntries = 2u;
  renderer.setGlyphResidencyBudgetForTesting(kMaximumEntries,
                                             /*maxEncodedBytes=*/1u << 30);

  const Frame frame = render(renderer, document);
  ASSERT_GT(nonTransparentPixels(frame.bitmap), 0u) << "Text did not render at all.";
  EXPECT_LE(renderer.residentGlyphCountForTesting(document), kMaximumEntries)
      << "A single frame must not retain more glyph entries than the configured admission cap.";
}

TEST_F(GeodeGlyphInstancingTest, MaterializationBudgetRejectsBeforeSecondOutlineDecode) {
  SVGDocument document = parse(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200"
           font-family="Noto Sans" font-size="48">
        <text x="10" y="80" fill="black">ab</text>
      </svg>)svg");

  RendererGeode renderer(sharedDevice());
  renderer.setTextMaterializationBudgetForTesting(
      {.uniqueOutlines = 1u,
       .commands = RendererTextMaterializationBudget::kMaximumCommands,
       .points = RendererTextMaterializationBudget::kMaximumPoints,
       .bytes = RendererTextMaterializationBudget::kMaximumBytes,
       .decodeWork = RendererTextMaterializationBudget::kMaximumDecodeWork},
      /*maximumGlyphOccurrences=*/2u);

  const Frame frame = render(renderer, document);
  const RendererResourceStats stats = renderer.resourceStats();
  EXPECT_TRUE(stats.textMaterializationBudgetSupported);
  EXPECT_EQ(stats.textUniqueOutlines, 1u);
  EXPECT_EQ(stats.textGlyphOccurrences, 2u);
  EXPECT_TRUE(stats.textMaterializationBudgetRejected);
  EXPECT_EQ(frame.counters.glyphResidencyUploads, 1u)
      << "The cap+1 glyph must be rejected before its outline reaches the cache miss builder.";
  EXPECT_EQ(renderer.residentGlyphCountForTesting(document), 1u);
}

TEST_F(GeodeGlyphInstancingTest, MaterializationBudgetIsSharedAcrossOffscreenRenderers) {
  SVGDocument firstDocument = parse(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200"
           font-family="Noto Sans" font-size="48">
        <text x="10" y="80" fill="black">a</text>
      </svg>)svg");
  SVGDocument secondDocument = parse(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200"
           font-family="Noto Sans" font-size="48">
        <text x="10" y="80" fill="black">b</text>
      </svg>)svg");

  RendererGeode renderer(sharedDevice());
  renderer.setTextMaterializationBudgetForTesting(
      {.uniqueOutlines = 1u,
       .commands = RendererTextMaterializationBudget::kMaximumCommands,
       .points = RendererTextMaterializationBudget::kMaximumPoints,
       .bytes = RendererTextMaterializationBudget::kMaximumBytes,
       .decodeWork = RendererTextMaterializationBudget::kMaximumDecodeWork},
      /*maximumGlyphOccurrences=*/2u);
  std::unique_ptr<RendererInterface> offscreen = renderer.createOffscreenInstance();
  ASSERT_NE(offscreen, nullptr);

  const Frame first = render(renderer, firstDocument);
  ASSERT_GT(nonTransparentPixels(first.bitmap), 0u);
  offscreen->draw(secondDocument);

  const RendererResourceStats stats = renderer.resourceStats();
  EXPECT_EQ(stats.textUniqueOutlines, 1u);
  EXPECT_EQ(stats.textGlyphOccurrences, 2u);
  EXPECT_TRUE(stats.textMaterializationBudgetRejected);
  EXPECT_EQ(renderer.residentGlyphCountForTesting(firstDocument), 1u);
  EXPECT_EQ(renderer.residentGlyphCountForTesting(secondDocument), 0u)
      << "The offscreen cap+1 miss must not decode or populate its document cache.";
}

TEST_F(GeodeGlyphInstancingTest, ResidentGlyphHitsStillRequireGeometrySubmissionAdmission) {
  SVGDocument document = parse(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200"
           font-family="Noto Sans" font-size="48">
        <text x="10" y="80" fill="black">a</text>
      </svg>)svg");

  RendererGeode renderer(sharedDevice());
  const Frame first = render(renderer, document);
  ASSERT_GT(nonTransparentPixels(first.bitmap), 0u);
  ASSERT_EQ(renderer.residentGlyphCountForTesting(document), 1u);

  renderer.setGeometryBudgetForTesting(/*maximumDraws=*/0u, /*maximumItems=*/1u << 20,
                                       /*maximumFrameBytes=*/64u << 20,
                                       /*maximumCacheBytes=*/64u << 20,
                                       /*maximumResidentBytes=*/64u << 20);
  const Frame rejected = render(renderer, document);
  EXPECT_EQ(rejected.counters.glyphResidencyHits, 1u)
      << "The second frame must exercise the resident-cache hit path.";
  EXPECT_TRUE(renderer.resourceStats().geometryBudgetRejected);
  EXPECT_EQ(nonTransparentPixels(rejected.bitmap), 0u)
      << "A resident hit must not bypass a rejected geometry submission.";
}

TEST_F(GeodeGlyphInstancingTest, SceneBatchChargesEachLogicalGlyphExactlyOnce) {
  SVGDocument document = parse(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200"
           font-family="Noto Sans" font-size="48">
        <text x="10" y="80" fill="black">eeee</text>
      </svg>)svg");

  RendererGeode renderer(sharedDevice());
  renderer.setGeometryBudgetForTesting(/*maximumDraws=*/4u, /*maximumItems=*/1u << 20,
                                       /*maximumFrameBytes=*/64u << 20,
                                       /*maximumCacheBytes=*/64u << 20,
                                       /*maximumResidentBytes=*/64u << 20);
  const Frame exact = render(renderer, document);
  ASSERT_GT(nonTransparentPixels(exact.bitmap), 0u);
  EXPECT_EQ(renderer.resourceStats().geometryDraws, 4u);
  EXPECT_FALSE(renderer.resourceStats().geometryBudgetRejected)
      << "The final batch draw must consume the four append-time reservations, not charge again.";

  renderer.setGeometryBudgetForTesting(/*maximumDraws=*/3u, /*maximumItems=*/1u << 20,
                                       /*maximumFrameBytes=*/64u << 20,
                                       /*maximumCacheBytes=*/64u << 20,
                                       /*maximumResidentBytes=*/64u << 20);
  const Frame capPlusOne = render(renderer, document);
  EXPECT_EQ(renderer.resourceStats().geometryDraws, 3u);
  EXPECT_TRUE(renderer.resourceStats().geometryBudgetRejected);
  EXPECT_GT(nonTransparentPixels(capPlusOne.bitmap), 0u)
      << "Accepted glyphs must remain drawable when the cap+1 occurrence is rejected.";
}

/// The cache key carries every parameter that changes the outline. A font-size
/// change alters the scale the outline is built at, so it must mint new entries
/// and draw the new size rather than serving the old geometry.
TEST_F(GeodeGlyphInstancingTest, FontSizeChangeInvalidatesResidentGlyphGeometry) {
  SVGDocument document = parse(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
        <text id="t" x="10" y="120" font-family="Noto Sans" font-size="24"
              fill="black">HH</text>
      </svg>)svg");

  RendererGeode renderer(sharedDevice());
  const Frame small = render(renderer, document);
  const size_t smallCovered = nonTransparentPixels(small.bitmap);
  ASSERT_GT(smallCovered, 0u) << "Text did not render at all.";
  ASSERT_EQ(renderer.residentGlyphCountForTesting(document), 1u);

  auto text = document.querySelector("#t");
  ASSERT_TRUE(text.has_value());
  text->setAttribute("font-size", "96");

  const Frame large = render(renderer, document);
  EXPECT_GT(large.counters.glyphResidencyUploads, 0u)
      << "A scale change must build a new outline rather than reuse the old entry.";
  EXPECT_EQ(renderer.residentGlyphCountForTesting(document), 2u)
      << "The two sizes are distinct glyph identities and must not share an entry.";
  EXPECT_GT(nonTransparentPixels(large.bitmap), smallCovered * 3u)
      << "The larger font must cover substantially more of the canvas; serving the cached "
         "small outline would keep the coverage the same.";

  // Returning to the original size renders the original geometry again. This
  // deliberately does not assert a cache hit: a style mutation re-resolves the
  // document's fonts, and a re-resolved font is a new identity, so the earlier
  // entry no longer matches. That costs a rebuild on a mutating document and
  // the stale entries age out through the residency budget; what must never
  // happen is the wrong geometry being served, which the coverage check below
  // is what pins.
  text->setAttribute("font-size", "24");
  const Frame backToSmall = render(renderer, document);
  EXPECT_EQ(nonTransparentPixels(backToSmall.bitmap), smallCovered)
      << "Returning to the original size must render the original geometry.";
}

/// Per-glyph `rotate` is part of glyph identity, so a rotated run is cached and
/// instanced exactly like an upright one: one angle costs one outline no matter
/// how many occurrences carry it.
TEST_F(GeodeGlyphInstancingTest, WholeRunRotationCostsOneResidentOutlinePerAngle) {
  SVGDocument document = parse(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200"
           font-family="Noto Sans" font-size="32">
        <text x="20" y="120" fill="black" rotate="30">eeee</text>
      </svg>)svg");

  RendererGeode renderer(sharedDevice());
  const Frame first = render(renderer, document);
  ASSERT_GT(nonTransparentPixels(first.bitmap), 0u) << "Rotated text did not render at all.";

  EXPECT_EQ(first.counters.glyphResidencyUploads, 1u)
      << "Four occurrences of one glyph at one angle must build one outline.";
  EXPECT_EQ(renderer.residentGlyphCountForTesting(document), 1u);

  const Frame second = render(renderer, document);
  EXPECT_EQ(second.counters.glyphResidencyUploads, 0u);
  EXPECT_EQ(second.counters.pathEncodes, 0u)
      << "An unchanged rotated text frame must re-encode nothing.";
  EXPECT_EQ(nonTransparentPixels(second.bitmap), nonTransparentPixels(first.bitmap));
}

/// Angle is part of the key, so glyphs that differ ONLY in rotation must not
/// share an outline - serving one angle's geometry for another would draw the
/// glyph tilted the wrong way.
TEST_F(GeodeGlyphInstancingTest, DistinctRotationsTakeDistinctResidentOutlines) {
  SVGDocument document = parse(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200"
           font-family="Noto Sans" font-size="32">
        <text x="20" y="120" fill="black" rotate="0 45 90">eee</text>
      </svg>)svg");

  RendererGeode renderer(sharedDevice());
  const Frame first = render(renderer, document);
  ASSERT_GT(nonTransparentPixels(first.bitmap), 0u) << "Rotated text did not render at all.";

  EXPECT_EQ(renderer.residentGlyphCountForTesting(document), 3u)
      << "One glyph at three angles is three glyph identities.";
  EXPECT_EQ(first.counters.glyphResidencyUploads, 3u);
}

/// A document that keeps mutating - the editor's typing and drag loop - must
/// not churn glyph identities when the mutation says nothing about the glyphs.
/// A style recompute re-resolves the text element's font, and the resolved font
/// entity is what `GlyphGeometryKey::fontId` carries, so an unstable entity
/// would silently give every glyph a new key on every keystroke: total misses,
/// every outline refetched and re-uploaded, and the superseded entries left to
/// be evicted. Recoloring changes no glyph geometry, so residency must hold at
/// the run's distinct-outline count with nothing re-derived and nothing
/// evicted.
TEST_F(GeodeGlyphInstancingTest, RepeatedMutationKeepsResidencyBounded) {
  SVGDocument document = parse(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
        <text id="t" x="10" y="120" font-family="Noto Sans" font-size="24"
              fill="black">Hello</text>
      </svg>)svg");

  auto text = document.querySelector("#t");
  ASSERT_TRUE(text.has_value());

  RendererGeode renderer(sharedDevice());
  // "Hello" resolves to four distinct glyph outlines, and a recolor changes
  // none of them, so this is the exact count for every round rather than a
  // ceiling: any other number means identities are being churned.
  constexpr size_t kDistinctGlyphs = 4u;

  uint64_t totalEvictions = 0;
  uint64_t uploadsAfterFirstFrame = 0;
  for (int round = 0; round < 20; ++round) {
    // Alternate the fill so every round is a real style mutation while the
    // glyph geometry stays identical.
    text->setAttribute("fill", (round % 2) == 0 ? "#101010" : "#202020");
    const Frame frame = render(renderer, document);
    ASSERT_GT(nonTransparentPixels(frame.bitmap), 0u)
        << "Text stopped rendering at mutation round " << round;
    totalEvictions += frame.counters.glyphResidencyEvictions;
    if (round > 0) {
      uploadsAfterFirstFrame += frame.counters.glyphResidencyUploads;
    }

    EXPECT_EQ(renderer.residentGlyphCountForTesting(document), kDistinctGlyphs)
        << "A recolor changed the resident glyph set at mutation round " << round;
  }

  EXPECT_EQ(uploadsAfterFirstFrame, 0u)
      << "A mutation that changes no glyph geometry re-derived an outline; the glyph key is not "
         "stable across style recomputes.";
  EXPECT_EQ(totalEvictions, 0u)
      << "Nothing was superseded, so nothing should have needed reclaiming.";
}

/// The counterpart to the recolor loop: a mutation that DOES change glyph
/// geometry legitimately produces new identities every round, and the residency
/// has to reclaim the superseded ones rather than growing without limit. This
/// is what keeps the eviction path exercised now that ordinary mutation no
/// longer churns.
TEST_F(GeodeGlyphInstancingTest, GlyphChurnStaysBoundedByEviction) {
  SVGDocument document = parse(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
        <text id="t" x="10" y="120" font-family="Noto Sans" font-size="24"
              fill="black">Hello</text>
      </svg>)svg");

  auto text = document.querySelector("#t");
  ASSERT_TRUE(text.has_value());

  RendererGeode renderer(sharedDevice());
  constexpr size_t kMaxEntries = 16u;
  constexpr size_t kDistinctGlyphsPerFrame = 4u;
  constexpr size_t kCeiling = kMaxEntries + kDistinctGlyphsPerFrame;
  renderer.setGlyphResidencyBudgetForTesting(kMaxEntries, /*maxEncodedBytes=*/1u << 30);

  uint64_t totalEvictions = 0;
  size_t settledCount = 0;
  for (int round = 0; round < 20; ++round) {
    // A new font size every round is a genuinely new outline scale, so each
    // round's glyphs are new identities and the previous round's are dead.
    text->setAttribute("font-size", std::to_string(20 + round));
    const Frame frame = render(renderer, document);
    ASSERT_GT(nonTransparentPixels(frame.bitmap), 0u)
        << "Text stopped rendering at mutation round " << round;
    totalEvictions += frame.counters.glyphResidencyEvictions;

    const size_t resident = renderer.residentGlyphCountForTesting(document);
    EXPECT_LE(resident, kCeiling) << "Residency exceeded its budget at mutation round " << round;

    // Once the churn has filled the budget, the count must stop climbing:
    // that, not the absolute number, is what says superseded identities are
    // reclaimed rather than stranded.
    if (round == 9) {
      settledCount = resident;
    } else if (round > 9) {
      EXPECT_LE(resident, settledCount)
          << "Residency grew after settling, at mutation round " << round;
    }
  }

  EXPECT_GT(totalEvictions, 0u)
      << "Glyph churn must be reclaimed by eviction, not accumulated. If this fails while "
         "the ceiling assertion passes, the churn stopped happening and this test is now "
         "measuring nothing.";
}

/// The narrowest statement of the same invariant, on the mutation an editor
/// drag actually performs: moving a `<text>` element changes where its glyphs
/// land, never which outlines they are. Every frame after the first must serve
/// all of them from residency, re-encoding and re-uploading nothing.
TEST_F(GeodeGlyphInstancingTest, RepositioningTextReusesEveryResidentGlyph) {
  SVGDocument document = parse(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
        <text id="t" x="10" y="120" font-family="Noto Sans" font-size="24"
              fill="black">Hello</text>
      </svg>)svg");

  auto text = document.querySelector("#t");
  ASSERT_TRUE(text.has_value());

  RendererGeode renderer(sharedDevice());
  const Frame first = render(renderer, document);
  ASSERT_GT(nonTransparentPixels(first.bitmap), 0u) << "Text did not render at all.";
  const size_t residentAfterFirst = renderer.residentGlyphCountForTesting(document);
  ASSERT_GT(residentAfterFirst, 0u) << "No glyphs became resident; the loop below proves nothing.";

  for (int round = 0; round < 16; ++round) {
    text->setAttribute("x", std::to_string(10 + round));
    const Frame frame = render(renderer, document);
    ASSERT_GT(nonTransparentPixels(frame.bitmap), 0u)
        << "Text stopped rendering at drag step " << round;

    EXPECT_EQ(frame.counters.glyphResidencyUploads, 0u)
        << "A move re-derived a glyph outline at drag step " << round;
    EXPECT_EQ(frame.counters.pathEncodes, 0u)
        << "A move re-encoded glyph geometry at drag step " << round;
    EXPECT_EQ(frame.counters.glyphResidencyEvictions, 0u)
        << "A move superseded a glyph identity at drag step " << round;
    EXPECT_EQ(renderer.residentGlyphCountForTesting(document), residentAfterFirst)
        << "Residency changed size on a move at drag step " << round;
  }
}

/// Reuse must not go so far that a genuinely different font keeps the old
/// glyphs. Switching `font-family` selects different outlines for the same
/// characters, so those are new identities that have to be built and drawn.
TEST_F(GeodeGlyphInstancingTest, FontFamilyChangeTakesNewGlyphIdentities) {
  SVGDocument document = parse(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
        <text id="t" x="10" y="120" font-family="Noto Sans" font-size="48"
              fill="black">Hello</text>
      </svg>)svg");

  auto text = document.querySelector("#t");
  ASSERT_TRUE(text.has_value());

  RendererGeode renderer(sharedDevice());
  const Frame sans = render(renderer, document);
  const size_t sansPixels = nonTransparentPixels(sans.bitmap);
  ASSERT_GT(sansPixels, 0u) << "Text did not render at all.";
  const size_t sansResident = renderer.residentGlyphCountForTesting(document);
  ASSERT_GT(sansResident, 0u);

  text->setAttribute("font-family", "Noto Serif");
  const Frame serif = render(renderer, document);
  EXPECT_GT(nonTransparentPixels(serif.bitmap), 0u) << "Text stopped rendering after the swap.";
  EXPECT_GT(serif.counters.glyphResidencyUploads, 0u)
      << "A different font must build its own outlines rather than reusing the previous font's.";
  EXPECT_GT(renderer.residentGlyphCountForTesting(document), sansResident)
      << "The serif glyphs are additional identities, not the sans ones renamed.";
  EXPECT_NE(nonTransparentPixels(serif.bitmap), sansPixels)
      << "The serif face drew exactly the sans coverage; the font swap did not reach the glyphs.";
}

}  // namespace
}  // namespace donner::svg
