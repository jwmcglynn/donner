/// @file
/// Tests for the toolbar tool-icon registry. Covers registry completeness
/// (every `ToolbarIcon` has embedded SVG art and a unique texture key) and a
/// raster snapshot through the shared `EmbeddedSvgIcon` COLOR Donner-render path
/// (each icon rasterizes to a distinct, non-empty TWO-TONE glyph: a solid black
/// core with a white outline, matching its OS cursor - QA-F7).

#include "donner/editor/ToolbarIconSet.h"

#include <gtest/gtest.h>

#include <set>

namespace donner::editor {
namespace {

std::size_t CountNonTransparentPixels(const svg::RendererBitmap& bitmap) {
  std::size_t count = 0;
  for (int y = 0; y < bitmap.dimensions.y; ++y) {
    const unsigned char* row =
        bitmap.pixels.data() + static_cast<std::size_t>(y) * bitmap.rowBytes;
    for (int x = 0; x < bitmap.dimensions.x; ++x) {
      if (row[static_cast<std::size_t>(x) * 4u + 3u] > 0) {
        ++count;
      }
    }
  }
  return count;
}

// Toolbar icons are rendered in full color (premultiplied RGBA) so their
// two-tone paint survives to the texture. Count near-opaque black-core and
// white-outline pixels to prove both tones are present.
std::size_t CountOpaqueBlackPixels(const svg::RendererBitmap& bitmap) {
  std::size_t count = 0;
  for (int y = 0; y < bitmap.dimensions.y; ++y) {
    const unsigned char* row =
        bitmap.pixels.data() + static_cast<std::size_t>(y) * bitmap.rowBytes;
    for (int x = 0; x < bitmap.dimensions.x; ++x) {
      const unsigned char* p = row + static_cast<std::size_t>(x) * 4u;
      if (p[3] > 180 && p[0] < 40 && p[1] < 40 && p[2] < 40) {
        ++count;
      }
    }
  }
  return count;
}

std::size_t CountOpaqueWhitePixels(const svg::RendererBitmap& bitmap) {
  std::size_t count = 0;
  for (int y = 0; y < bitmap.dimensions.y; ++y) {
    const unsigned char* row =
        bitmap.pixels.data() + static_cast<std::size_t>(y) * bitmap.rowBytes;
    for (int x = 0; x < bitmap.dimensions.x; ++x) {
      const unsigned char* p = row + static_cast<std::size_t>(x) * 4u;
      if (p[3] > 180 && p[0] > 220 && p[1] > 220 && p[2] > 220) {
        ++count;
      }
    }
  }
  return count;
}

TEST(ToolbarIconSet, EveryIconHasArtAndAUniqueTextureKey) {
  std::set<std::uint64_t> keys;
  for (ToolbarIcon icon : kToolbarIcons) {
    EXPECT_FALSE(ToolbarIconSvg(icon).empty()) << "icon index " << static_cast<int>(icon);
    EXPECT_TRUE(keys.insert(ToolbarIconTextureKey(icon)).second)
        << "duplicate texture key for icon index " << static_cast<int>(icon);
  }
  EXPECT_EQ(keys.size(), kToolbarIcons.size());
}

TEST(ToolbarIconSet, EveryIconRastersToANonEmptyTwoToneGlyph) {
  for (ToolbarIcon icon : kToolbarIcons) {
    const std::optional<svg::RendererBitmap>& bitmap = CachedToolbarIconBitmap(icon);
    ASSERT_TRUE(bitmap.has_value()) << "icon index " << static_cast<int>(icon);
    EXPECT_GT(bitmap->dimensions.x, 0);
    EXPECT_GT(bitmap->dimensions.y, 0);
    EXPECT_GT(CountNonTransparentPixels(*bitmap), 40u) << "icon index " << static_cast<int>(icon);
    // Both tones present: a solid black core and a white outline, so the toolbar
    // icon reads as the same two-tone glyph as the cursor.
    EXPECT_GT(CountOpaqueBlackPixels(*bitmap), 25u) << "icon index " << static_cast<int>(icon);
    EXPECT_GT(CountOpaqueWhitePixels(*bitmap), 8u) << "icon index " << static_cast<int>(icon);
  }
}

TEST(ToolbarIconSet, DistinctIconsRasterizeToDistinctGlyphs) {
  const std::optional<svg::RendererBitmap>& select = CachedToolbarIconBitmap(ToolbarIcon::Select);
  const std::optional<svg::RendererBitmap>& text = CachedToolbarIconBitmap(ToolbarIcon::Text);
  ASSERT_TRUE(select.has_value());
  ASSERT_TRUE(text.has_value());
  EXPECT_NE(select->pixels, text->pixels);
}

}  // namespace
}  // namespace donner::editor
