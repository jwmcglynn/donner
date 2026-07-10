/// @file
/// Tests for the toolbar tool-icon registry. Covers registry completeness
/// (every `ToolbarIcon` has embedded SVG art and a unique texture key) and a
/// raster snapshot through the shared `EmbeddedSvgIcon` Donner-render path.
/// Each icon must remain distinct, unclipped, and two-tone after rasterization.

#include "donner/editor/ToolbarIconSet.h"

#include <gtest/gtest.h>

#include <set>

namespace donner::editor {
namespace {

std::size_t CountVisiblePixels(const svg::RendererBitmap& bitmap) {
  std::size_t count = 0;
  for (int y = 0; y < bitmap.dimensions.y; ++y) {
    const unsigned char* row = bitmap.pixels.data() + static_cast<std::size_t>(y) * bitmap.rowBytes;
    for (int x = 0; x < bitmap.dimensions.x; ++x) {
      const unsigned char* pixel = row + static_cast<std::size_t>(x) * 4u;
      if (pixel[3] > 0) {
        ++count;
      }
    }
  }
  return count;
}

std::size_t CountOpaqueBlackPixels(const svg::RendererBitmap& bitmap) {
  std::size_t count = 0;
  for (int y = 0; y < bitmap.dimensions.y; ++y) {
    const unsigned char* row = bitmap.pixels.data() + static_cast<std::size_t>(y) * bitmap.rowBytes;
    for (int x = 0; x < bitmap.dimensions.x; ++x) {
      const unsigned char* pixel = row + static_cast<std::size_t>(x) * 4u;
      if (pixel[3] > 220 && pixel[0] < 35 && pixel[1] < 35 && pixel[2] < 35) {
        ++count;
      }
    }
  }
  return count;
}

std::size_t CountOpaqueWhitePixels(const svg::RendererBitmap& bitmap) {
  std::size_t count = 0;
  for (int y = 0; y < bitmap.dimensions.y; ++y) {
    const unsigned char* row = bitmap.pixels.data() + static_cast<std::size_t>(y) * bitmap.rowBytes;
    for (int x = 0; x < bitmap.dimensions.x; ++x) {
      const unsigned char* pixel = row + static_cast<std::size_t>(x) * 4u;
      if (pixel[3] > 220 && pixel[0] > 220 && pixel[1] > 220 && pixel[2] > 220) {
        ++count;
      }
    }
  }
  return count;
}

bool HasVisibleEdgePixel(const svg::RendererBitmap& bitmap) {
  for (int x = 0; x < bitmap.dimensions.x; ++x) {
    const std::size_t top = static_cast<std::size_t>(x) * 4u + 3u;
    const std::size_t bottom = static_cast<std::size_t>(bitmap.dimensions.y - 1) * bitmap.rowBytes +
                               static_cast<std::size_t>(x) * 4u + 3u;
    if (bitmap.pixels[top] != 0 || bitmap.pixels[bottom] != 0) {
      return true;
    }
  }
  for (int y = 0; y < bitmap.dimensions.y; ++y) {
    const std::size_t left = static_cast<std::size_t>(y) * bitmap.rowBytes + 3u;
    const std::size_t right = static_cast<std::size_t>(y) * bitmap.rowBytes +
                              static_cast<std::size_t>(bitmap.dimensions.x - 1) * 4u + 3u;
    if (bitmap.pixels[left] != 0 || bitmap.pixels[right] != 0) {
      return true;
    }
  }
  return false;
}

std::size_t CountOpaqueBlackPixelsInLowerRight(const svg::RendererBitmap& bitmap) {
  std::size_t count = 0;
  const int minX = bitmap.dimensions.x / 2;
  const int minY = bitmap.dimensions.y * 7 / 10;
  for (int y = minY; y < bitmap.dimensions.y; ++y) {
    const unsigned char* row = bitmap.pixels.data() + static_cast<std::size_t>(y) * bitmap.rowBytes;
    for (int x = minX; x < bitmap.dimensions.x; ++x) {
      const unsigned char* pixel = row + static_cast<std::size_t>(x) * 4u;
      if (pixel[3] > 220 && pixel[0] < 35 && pixel[1] < 35 && pixel[2] < 35) {
        ++count;
      }
    }
  }
  return count;
}

std::size_t CountOpaqueBlackPixelsAdjacentToTransparency(const svg::RendererBitmap& bitmap) {
  std::size_t count = 0;
  for (int y = 1; y + 1 < bitmap.dimensions.y; ++y) {
    const unsigned char* row = bitmap.pixels.data() + static_cast<std::size_t>(y) * bitmap.rowBytes;
    for (int x = 1; x + 1 < bitmap.dimensions.x; ++x) {
      const unsigned char* pixel = row + static_cast<std::size_t>(x) * 4u;
      if (pixel[3] <= 220 || pixel[0] >= 35 || pixel[1] >= 35 || pixel[2] >= 35) {
        continue;
      }
      bool adjacentToTransparency = false;
      for (int dy = -1; dy <= 1; ++dy) {
        const unsigned char* neighborRow =
            bitmap.pixels.data() + static_cast<std::size_t>(y + dy) * bitmap.rowBytes;
        for (int dx = -1; dx <= 1; ++dx) {
          const unsigned char* neighbor = neighborRow + static_cast<std::size_t>(x + dx) * 4u;
          adjacentToTransparency = adjacentToTransparency || neighbor[3] < 32;
        }
      }
      count += adjacentToTransparency ? 1u : 0u;
    }
  }
  return count;
}

Vector2d VisiblePixelCentroid(const svg::RendererBitmap& bitmap) {
  double weightedX = 0.0;
  double weightedY = 0.0;
  double totalAlpha = 0.0;
  for (int y = 0; y < bitmap.dimensions.y; ++y) {
    const unsigned char* row = bitmap.pixels.data() + static_cast<std::size_t>(y) * bitmap.rowBytes;
    for (int x = 0; x < bitmap.dimensions.x; ++x) {
      const double alpha = static_cast<double>(row[static_cast<std::size_t>(x) * 4u + 3u]);
      weightedX += (static_cast<double>(x) + 0.5) * alpha;
      weightedY += (static_cast<double>(y) + 0.5) * alpha;
      totalAlpha += alpha;
    }
  }
  return Vector2d(weightedX / totalAlpha, weightedY / totalAlpha);
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

TEST(ToolbarIconSet, EveryIconRastersToUnclippedTwoToneArtwork) {
  for (ToolbarIcon icon : kToolbarIcons) {
    const std::optional<svg::RendererBitmap>& bitmap = CachedToolbarIconBitmap(icon);
    ASSERT_TRUE(bitmap.has_value()) << "icon index " << static_cast<int>(icon);
    EXPECT_GT(bitmap->dimensions.x, 0);
    EXPECT_GT(bitmap->dimensions.y, 0);
    EXPECT_GT(CountVisiblePixels(*bitmap), 100u) << "icon index " << static_cast<int>(icon);
    EXPECT_GT(CountOpaqueBlackPixels(*bitmap), 20u) << "icon index " << static_cast<int>(icon);
    EXPECT_GT(CountOpaqueWhitePixels(*bitmap), 20u) << "icon index " << static_cast<int>(icon);
    EXPECT_FALSE(HasVisibleEdgePixel(*bitmap)) << "icon index " << static_cast<int>(icon);
  }
}

TEST(ToolbarIconSet, DistinctIconsRasterizeToDistinctMasks) {
  const std::optional<svg::RendererBitmap>& select = CachedToolbarIconBitmap(ToolbarIcon::Select);
  const std::optional<svg::RendererBitmap>& text = CachedToolbarIconBitmap(ToolbarIcon::Text);
  ASSERT_TRUE(select.has_value());
  ASSERT_TRUE(text.has_value());
  EXPECT_NE(select->pixels, text->pixels);
}

TEST(ToolbarIconSet, SelectIconUsesTaillessCursorSilhouette) {
  const std::optional<svg::RendererBitmap>& select = CachedToolbarIconBitmap(ToolbarIcon::Select);
  ASSERT_TRUE(select.has_value());
  EXPECT_EQ(CountOpaqueBlackPixelsInLowerRight(*select), 0u);
  EXPECT_EQ(CountOpaqueBlackPixelsAdjacentToTransparency(*select), 0u);

  const Vector2d centroid = VisiblePixelCentroid(*select);
  EXPECT_NEAR(centroid.x, static_cast<double>(select->dimensions.x) * 0.5, 2.0);
  EXPECT_NEAR(centroid.y, static_cast<double>(select->dimensions.y) * 0.5, 2.0);
}

}  // namespace
}  // namespace donner::editor
