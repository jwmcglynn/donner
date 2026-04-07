#include "donner/svg/tool/DonnerSvgToolUtils.h"

#include <span>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/TerminalImageViewer.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace donner::svg {
namespace {

using testing::Optional;

SVGDocument ParseDoc(std::string_view svg) {
  auto parsed = parser::SVGParser::ParseSVG(svg);
  EXPECT_FALSE(parsed.hasError()) << parsed.error();
  return std::move(parsed.result());
}

TEST(DonnerSvgToolUtils, BuildsCssSelectorPathWithIdsAndNthChild) {
  const char* kSvg = R"(
      <svg xmlns="http://www.w3.org/2000/svg" id="root">
        <g id="group-a">
          <rect id="rect-a" width="10" height="10" />
          <rect class="primary" width="10" height="10" />
        </g>
      </svg>
  )";

  SVGDocument doc = ParseDoc(kSvg);
  auto maybeTarget = doc.querySelector(".primary");
  ASSERT_THAT(maybeTarget, Optional(testing::_));

  EXPECT_EQ(BuildCssSelectorPath(*maybeTarget), "svg#root > g#group-a > rect.primary:nth-child(2)");
}

// Helper to create a black RGBA bitmap.
RendererBitmap MakeBlackBitmap(int width, int height) {
  RendererBitmap bmp;
  bmp.dimensions = Vector2i(width, height);
  bmp.rowBytes = static_cast<size_t>(width) * 4;
  bmp.pixels.assign(bmp.rowBytes * static_cast<size_t>(height), 0);
  return bmp;
}

// Sample the bitmap using TerminalImageViewer and return which sub-pixels are blue.
// Returns a 2D grid of booleans indexed as [subY][subX].
std::vector<std::vector<bool>> SampleSubPixelColors(const RendererBitmap& bmp,
                                                     const SampledImageInfo& info) {
  TerminalImageViewerConfig config;
  config.pixelMode = TerminalPixelMode::kQuarterPixel;
  config.autoScale = false;
  // Set scale so that we get exactly the expected columns/rows.
  // scale = (columns * 2) / imageWidth (derived from sampler's formula).
  config.scale = static_cast<double>(info.columns * 2) / static_cast<double>(bmp.dimensions.x);
  config.verticalScaleFactor =
      static_cast<double>(info.rows * 2) /
      (static_cast<double>(bmp.dimensions.y) * config.scale);

  TerminalImageView view;
  view.data = std::span<const uint8_t>(bmp.pixels.data(), bmp.pixels.size());
  view.width = bmp.dimensions.x;
  view.height = bmp.dimensions.y;
  view.strideInPixels = bmp.rowBytes / 4;

  TerminalImageViewer viewer;
  TerminalImage sampled = viewer.sampleImage(view, config);

  // Extract sub-pixel colors. Each cell is 2x2 sub-pixels in quarter mode.
  const int subW = sampled.columns * 2;
  const int subH = sampled.rows * 2;
  std::vector<std::vector<bool>> grid(subH, std::vector<bool>(subW, false));

  for (int row = 0; row < sampled.rows; ++row) {
    for (int col = 0; col < sampled.columns; ++col) {
      const TerminalCell& cell = sampled.cellAt(col, row);
      auto isBlueColor = [](const css::RGBA& c) {
        // The sampler samples a single pixel per sub-pixel, so it should be exact blue.
        return c.r == 0x44 && c.g == 0x88 && c.b == 0xff && c.a == 0xff;
      };
      grid[row * 2 + 0][col * 2 + 0] = isBlueColor(cell.quarter.topLeft);
      grid[row * 2 + 0][col * 2 + 1] = isBlueColor(cell.quarter.topRight);
      grid[row * 2 + 1][col * 2 + 0] = isBlueColor(cell.quarter.bottomLeft);
      grid[row * 2 + 1][col * 2 + 1] = isBlueColor(cell.quarter.bottomRight);
    }
  }

  return grid;
}

// Render grid as ASCII for debugging.
std::string GridToString(const std::vector<std::vector<bool>>& grid) {
  std::string result;
  for (const auto& row : grid) {
    for (bool b : row) {
      result += b ? '#' : '.';
    }
    result += '\n';
  }
  return result;
}

TEST(CompositeAABBRect, RectIsExactlyOneSubPixelWide) {
  // 100x100 image, sampled into 10x10 terminal cells = 20x20 sub-pixels.
  // xScale = yScale = 100/20 = 5.0 pixels per sub-pixel.
  constexpr int kImgSize = 100;
  constexpr int kCells = 10;
  SampledImageInfo info{kCells, kCells, 5.0, 5.0};

  // AABB from (20, 20) to (70, 70) — should land on sub-pixel boundaries exactly.
  // Sub-pixel 4 covers pixels [20, 25), sub-pixel 13 covers [65, 70).
  RendererBitmap bmp = MakeBlackBitmap(kImgSize, kImgSize);
  CompositeAABBRect(bmp, Boxd(Vector2d(20.0, 20.0), Vector2d(70.0, 70.0)), info);

  auto grid = SampleSubPixelColors(bmp, info);
  ASSERT_EQ(grid.size(), 20u);
  ASSERT_EQ(grid[0].size(), 20u);

  // Expected: a rect outline from sub-pixel (4,4) to (13,13).
  // Top edge: row 4, columns 4..13 should be blue.
  // Bottom edge: row 13, columns 4..13 should be blue.
  // Left edge: column 4, rows 5..12 should be blue.
  // Right edge: column 13, rows 5..12 should be blue.

  auto msg = [&]() { return "\n" + GridToString(grid); };

  // Check top edge.
  for (int x = 0; x < 20; ++x) {
    EXPECT_EQ(grid[4][x], x >= 4 && x <= 13) << "top edge at x=" << x << msg();
  }

  // Check bottom edge.
  for (int x = 0; x < 20; ++x) {
    EXPECT_EQ(grid[13][x], x >= 4 && x <= 13) << "bottom edge at x=" << x << msg();
  }

  // Check left and right edges (between top and bottom, exclusive).
  for (int y = 5; y <= 12; ++y) {
    for (int x = 0; x < 20; ++x) {
      EXPECT_EQ(grid[y][x], x == 4 || x == 13) << "side edge at (" << x << "," << y << ")" << msg();
    }
  }

  // Check that rows outside the rect are empty.
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 20; ++x) {
      EXPECT_FALSE(grid[y][x]) << "above rect at (" << x << "," << y << ")" << msg();
    }
  }
  for (int y = 14; y < 20; ++y) {
    for (int x = 0; x < 20; ++x) {
      EXPECT_FALSE(grid[y][x]) << "below rect at (" << x << "," << y << ")" << msg();
    }
  }
}

TEST(CompositeAABBRect, NonAlignedBoundsSnapToNearestSubPixel) {
  // 100x100 image, 10x10 cells = 20x20 sub-pixels.
  // Cell c samples startPixel = int(c * 2 * 5.0), sub-pixels at startPixel and startPixel+1.
  // Sub-pixels sample pixels: 0,1, 10,11, 20,21, 30,31, 40,41, 50,51, 60,61, 70,71, ...
  // AABB from (22, 22) to (67, 67):
  //   Left: pixel 22 → nearest sub-pixel whose sampled pixel <= 22 is sub-pixel 5 (pixel 21)
  //   Top: same → sub-pixel 5
  //   Right: pixel 66 (last inside) → sub-pixel 13 (pixel 61)
  //   Bottom: same → sub-pixel 13
  constexpr int kImgSize = 100;
  constexpr int kCells = 10;
  SampledImageInfo info{kCells, kCells, 5.0, 5.0};

  RendererBitmap bmp = MakeBlackBitmap(kImgSize, kImgSize);
  CompositeAABBRect(bmp, Boxd(Vector2d(22.0, 22.0), Vector2d(67.0, 67.0)), info);

  auto grid = SampleSubPixelColors(bmp, info);

  auto msg = [&]() { return "\n" + GridToString(grid); };

  // Rect from sub-pixel (5,5) to (13,13).
  for (int x = 0; x < 20; ++x) {
    EXPECT_EQ(grid[5][x], x >= 5 && x <= 13) << "top edge at x=" << x << msg();
    EXPECT_EQ(grid[13][x], x >= 5 && x <= 13) << "bottom edge at x=" << x << msg();
  }
  for (int y = 6; y <= 12; ++y) {
    for (int x = 0; x < 20; ++x) {
      EXPECT_EQ(grid[y][x], x == 5 || x == 13) << "side at (" << x << "," << y << ")" << msg();
    }
  }
}

}  // namespace
}  // namespace donner::svg
