/// @file
/// Tests for the toolbar tool-icon registry. Covers registry completeness
/// (every `ToolbarIcon` has embedded SVG art and a unique texture key) and a
/// raster snapshot through the shared `EmbeddedSvgIcon` Donner-render path
/// (each icon rasterizes to a distinct, non-empty white tintable mask).

#include "donner/editor/ToolbarIconSet.h"

#include <gtest/gtest.h>

#include <set>

namespace donner::editor {
namespace {

std::size_t CountMaskPixels(const svg::RendererBitmap& bitmap) {
  std::size_t count = 0;
  for (int y = 0; y < bitmap.dimensions.y; ++y) {
    const unsigned char* row =
        bitmap.pixels.data() + static_cast<std::size_t>(y) * bitmap.rowBytes;
    for (int x = 0; x < bitmap.dimensions.x; ++x) {
      const unsigned char* pixel = row + static_cast<std::size_t>(x) * 4u;
      const unsigned char alpha = pixel[3];
      if (alpha > 0) {
        // NormalizeIconBitmapToTintableAlphaMask collapses RGB to the alpha, so
        // every covered pixel is a white premultiplied mask.
        EXPECT_EQ(pixel[0], alpha);
        EXPECT_EQ(pixel[1], alpha);
        EXPECT_EQ(pixel[2], alpha);
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

TEST(ToolbarIconSet, EveryIconRastersToANonEmptyTintableMask) {
  for (ToolbarIcon icon : kToolbarIcons) {
    const std::optional<svg::RendererBitmap>& bitmap = CachedToolbarIconBitmap(icon);
    ASSERT_TRUE(bitmap.has_value()) << "icon index " << static_cast<int>(icon);
    EXPECT_GT(bitmap->dimensions.x, 0);
    EXPECT_GT(bitmap->dimensions.y, 0);
    EXPECT_GT(CountMaskPixels(*bitmap), 20u) << "icon index " << static_cast<int>(icon);
  }
}

TEST(ToolbarIconSet, DistinctIconsRasterizeToDistinctMasks) {
  const std::optional<svg::RendererBitmap>& select = CachedToolbarIconBitmap(ToolbarIcon::Select);
  const std::optional<svg::RendererBitmap>& text = CachedToolbarIconBitmap(ToolbarIcon::Text);
  ASSERT_TRUE(select.has_value());
  ASSERT_TRUE(text.has_value());
  EXPECT_NE(select->pixels, text->pixels);
}

}  // namespace
}  // namespace donner::editor
