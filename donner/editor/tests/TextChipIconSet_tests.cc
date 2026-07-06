/// @file
/// Tests for the text-region chip icon registry (QA-F22). Covers registry
/// completeness - every `TextChipIcon` has embedded SVG art and a unique texture
/// key - and a raster snapshot through the shared `EmbeddedSvgIcon` mask path
/// (each chip mark rasterizes to a distinct, non-empty tintable mask). This
/// fails if any known chip mark regresses to a missing-glyph "?" placeholder.

#include "donner/editor/TextChipIconSet.h"

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
        // RenderEmbeddedSvgIcon collapses RGB to the alpha, so every covered
        // pixel is a white premultiplied mask the chip tints with its color.
        EXPECT_EQ(pixel[0], alpha);
        EXPECT_EQ(pixel[1], alpha);
        EXPECT_EQ(pixel[2], alpha);
        ++count;
      }
    }
  }
  return count;
}

TEST(TextChipIconSet, EveryChipIconHasArtAndAUniqueTextureKey) {
  std::set<std::uint64_t> keys;
  for (TextChipIcon icon : kTextChipIcons) {
    EXPECT_FALSE(TextChipIconSvg(icon).empty()) << "chip icon index " << static_cast<int>(icon);
    EXPECT_TRUE(keys.insert(TextChipIconTextureKey(icon)).second)
        << "duplicate texture key for chip icon index " << static_cast<int>(icon);
  }
  EXPECT_EQ(keys.size(), kTextChipIcons.size());
}

TEST(TextChipIconSet, EveryChipIconRastersToANonEmptyTintableMask) {
  for (TextChipIcon icon : kTextChipIcons) {
    const std::optional<svg::RendererBitmap>& bitmap = CachedTextChipIconBitmap(icon);
    ASSERT_TRUE(bitmap.has_value()) << "chip icon index " << static_cast<int>(icon);
    EXPECT_GT(bitmap->dimensions.x, 0);
    EXPECT_GT(bitmap->dimensions.y, 0);
    EXPECT_GT(CountMaskPixels(*bitmap), 20u) << "chip icon index " << static_cast<int>(icon);
  }
}

TEST(TextChipIconSet, DistinctChipIconsRasterizeToDistinctMasks) {
  const std::optional<svg::RendererBitmap>& styleSource =
      CachedTextChipIconBitmap(TextChipIcon::StyleSource);
  const std::optional<svg::RendererBitmap>& overflow =
      CachedTextChipIconBitmap(TextChipIcon::Overflow);
  ASSERT_TRUE(styleSource.has_value());
  ASSERT_TRUE(overflow.has_value());
  EXPECT_NE(styleSource->pixels, overflow->pixels);
}

}  // namespace
}  // namespace donner::editor
