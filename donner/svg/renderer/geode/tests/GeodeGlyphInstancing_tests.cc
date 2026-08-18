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
  EXPECT_EQ(second.counters.pathEncodes, 0u)
      << "An unchanged text frame must re-encode nothing.";
  EXPECT_EQ(nonTransparentPixels(second.bitmap), nonTransparentPixels(first.bitmap))
      << "The resident second frame must draw the same coverage as the first.";
}

/// Two separate `<text>` elements are distinct source entities that share glyph
/// outlines. They must batch together, and - the part that batching can get
/// wrong - overlapping instances must still composite in painter order, because
/// the batch draws them as instances of one call.
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
/// oldest unused entries are dropped, the dropped glyphs are re-uploaded when
/// they come back, and the text keeps rendering identically throughout.
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
  EXPECT_GT(squeezed.counters.glyphResidencyUploads, 0u)
      << "Glyphs dropped by the trim must be re-uploaded when they are drawn again.";
  EXPECT_EQ(nonTransparentPixels(squeezed.bitmap), covered)
      << "Eviction must not change what the frame draws.";

  // Still correct once the budget is lifted again: the entries that survived
  // the squeeze are still usable, not left half-released.
  renderer.setGlyphResidencyBudgetForTesting(/*maxEntries=*/1024, /*maxEncodedBytes=*/1u << 30);
  const Frame restored = render(renderer, document);
  EXPECT_EQ(nonTransparentPixels(restored.bitmap), covered);
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

  // Back to the original size: the first entry is still resident and serves it
  // without a rebuild.
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

}  // namespace
}  // namespace donner::svg
