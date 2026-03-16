/**
 * @file Tests for renderer error and edge-case paths.
 *
 * Exercises failure modes: empty snapshots, invalid file paths, broken SVG references,
 * degenerate geometry, and RendererImageIO edge cases.
 */

#include <gtest/gtest.h>

#include <filesystem>

#include "donner/svg/SVG.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RendererImageIO.h"

namespace donner::svg {
namespace {

SVGDocument ParseDocument(std::string_view svgSource) {
  parser::SVGParser::Options options;
  ParseResult<SVGDocument> maybeDocument = parser::SVGParser::ParseSVG(svgSource, nullptr, options);
  EXPECT_FALSE(maybeDocument.hasError());
  return std::move(maybeDocument.result());
}

// --- Renderer save/snapshot error paths ---

TEST(RendererErrorPathsTest, SaveBeforeDrawReturnsFalse) {
  Renderer renderer;
  // No draw() called → snapshot is empty → save returns false.
  const std::filesystem::path outputPath =
      std::filesystem::path(::testing::TempDir()) / "no_draw.png";
  EXPECT_FALSE(renderer.save(outputPath.c_str()));
}

TEST(RendererErrorPathsTest, SnapshotBeforeDrawIsEmpty) {
  Renderer renderer;
  const RendererBitmap snapshot = renderer.takeSnapshot();
  EXPECT_TRUE(snapshot.empty());
}

TEST(RendererErrorPathsTest, SaveToInvalidPathReturnsFalse) {
  SVGDocument document = ParseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="4" height="4">
      <rect width="4" height="4" fill="red"/>
    </svg>
  )svg");

  Renderer renderer;
  renderer.draw(document);

  // Write to a path that doesn't exist.
  EXPECT_FALSE(renderer.save("/nonexistent/directory/output.png"));
}

// --- RendererImageIO error paths ---

TEST(RendererImageIOTest, WritePngToInvalidPath) {
  std::vector<uint8_t> pixels(4 * 4 * 4, 128);  // 4x4 RGBA
  EXPECT_FALSE(
      RendererImageIO::writeRgbaPixelsToPngFile("/no/such/dir/test.png", pixels, 4, 4, 0));
}

TEST(RendererImageIOTest, WritePngToMemory) {
  std::vector<uint8_t> pixels(2 * 2 * 4, 255);  // 2x2 white RGBA
  std::vector<uint8_t> encoded = RendererImageIO::writeRgbaPixelsToPngMemory(pixels, 2, 2, 0);
  EXPECT_FALSE(encoded.empty());
  // PNG magic bytes.
  ASSERT_GE(encoded.size(), 8u);
  EXPECT_EQ(encoded[0], 0x89);
  EXPECT_EQ(encoded[1], 'P');
  EXPECT_EQ(encoded[2], 'N');
  EXPECT_EQ(encoded[3], 'G');
}

TEST(RendererImageIOTest, WritePngToFileAndVerify) {
  const std::filesystem::path outputPath =
      std::filesystem::path(::testing::TempDir()) / "image_io_test.png";
  std::vector<uint8_t> pixels(3 * 3 * 4, 100);  // 3x3 grey RGBA
  EXPECT_TRUE(RendererImageIO::writeRgbaPixelsToPngFile(outputPath.c_str(), pixels, 3, 3, 0));
  ASSERT_TRUE(std::filesystem::exists(outputPath));
  EXPECT_GT(std::filesystem::file_size(outputPath), 0u);
}

TEST(RendererImageIOTest, WritePngWithCustomStride) {
  // 2x2 image with stride equal to width (no extra padding).
  // The API asserts that rgbaPixels.size() == width * height * 4.
  std::vector<uint8_t> pixels(2 * 2 * 4, 200);  // 2 pixels per row × 2 rows × 4 bytes/pixel
  std::vector<uint8_t> encoded = RendererImageIO::writeRgbaPixelsToPngMemory(pixels, 2, 2, 2);
  EXPECT_FALSE(encoded.empty());
}

// --- Rendering broken/degenerate SVGs ---

TEST(RendererErrorPathsTest, EmptySvgDocument) {
  SVGDocument document = ParseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="8" height="8"/>
  )svg");

  Renderer renderer;
  renderer.draw(document);

  const RendererBitmap snapshot = renderer.takeSnapshot();
  // Empty document should still produce a valid (transparent) bitmap.
  EXPECT_FALSE(snapshot.empty());
  EXPECT_EQ(snapshot.dimensions, Vector2i(8, 8));
}

TEST(RendererErrorPathsTest, BrokenGradientReference) {
  // fill references a gradient that doesn't exist.
  SVGDocument document = ParseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
      <rect width="8" height="8" fill="url(#nonexistent)"/>
    </svg>
  )svg");

  Renderer renderer;
  renderer.draw(document);

  const RendererBitmap snapshot = renderer.takeSnapshot();
  EXPECT_FALSE(snapshot.empty());
}

TEST(RendererErrorPathsTest, EmptyFilterPrimitive) {
  // Filter with no primitives.
  SVGDocument document = ParseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
      <defs>
        <filter id="empty"/>
      </defs>
      <rect width="8" height="8" fill="green" filter="url(#empty)"/>
    </svg>
  )svg");

  Renderer renderer;
  renderer.draw(document);

  const RendererBitmap snapshot = renderer.takeSnapshot();
  EXPECT_FALSE(snapshot.empty());
}

TEST(RendererErrorPathsTest, ZeroDimensionViewBox) {
  SVGDocument document = ParseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="8" height="8" viewBox="0 0 0 0">
      <rect width="8" height="8" fill="blue"/>
    </svg>
  )svg");

  Renderer renderer;
  renderer.draw(document);

  const RendererBitmap snapshot = renderer.takeSnapshot();
  EXPECT_FALSE(snapshot.empty());
}

TEST(RendererErrorPathsTest, GradientWithNoStops) {
  SVGDocument document = ParseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
      <defs>
        <linearGradient id="empty-grad"/>
      </defs>
      <rect width="8" height="8" fill="url(#empty-grad)"/>
    </svg>
  )svg");

  Renderer renderer;
  renderer.draw(document);

  const RendererBitmap snapshot = renderer.takeSnapshot();
  EXPECT_FALSE(snapshot.empty());
}

TEST(RendererErrorPathsTest, SelfReferencingClipPath) {
  // Clip path that references itself — should be handled gracefully.
  SVGDocument document = ParseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
      <defs>
        <clipPath id="selfref" clip-path="url(#selfref)">
          <rect width="4" height="4"/>
        </clipPath>
      </defs>
      <rect width="8" height="8" fill="red" clip-path="url(#selfref)"/>
    </svg>
  )svg");

  Renderer renderer;
  renderer.draw(document);

  const RendererBitmap snapshot = renderer.takeSnapshot();
  EXPECT_FALSE(snapshot.empty());
}

TEST(RendererErrorPathsTest, ZeroSizeRect) {
  SVGDocument document = ParseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
      <rect width="0" height="0" fill="red"/>
    </svg>
  )svg");

  Renderer renderer;
  renderer.draw(document);

  const RendererBitmap snapshot = renderer.takeSnapshot();
  EXPECT_FALSE(snapshot.empty());
}

TEST(RendererErrorPathsTest, NegativeRadiiEllipse) {
  // Negative radii should be treated as invalid/zero.
  SVGDocument document = ParseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
      <ellipse cx="4" cy="4" rx="-2" ry="-2" fill="green"/>
    </svg>
  )svg");

  Renderer renderer;
  renderer.draw(document);

  const RendererBitmap snapshot = renderer.takeSnapshot();
  EXPECT_FALSE(snapshot.empty());
}

TEST(RendererErrorPathsTest, PatternWithZeroSize) {
  SVGDocument document = ParseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
      <defs>
        <pattern id="zero-pat" width="0" height="0">
          <rect width="1" height="1" fill="blue"/>
        </pattern>
      </defs>
      <rect width="8" height="8" fill="url(#zero-pat)"/>
    </svg>
  )svg");

  Renderer renderer;
  renderer.draw(document);

  const RendererBitmap snapshot = renderer.takeSnapshot();
  EXPECT_FALSE(snapshot.empty());
}

TEST(RendererErrorPathsTest, MaskWithNoContent) {
  SVGDocument document = ParseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
      <defs>
        <mask id="empty-mask"/>
      </defs>
      <rect width="8" height="8" fill="red" mask="url(#empty-mask)"/>
    </svg>
  )svg");

  Renderer renderer;
  renderer.draw(document);

  const RendererBitmap snapshot = renderer.takeSnapshot();
  EXPECT_FALSE(snapshot.empty());
}

TEST(RendererErrorPathsTest, DisplayNoneElement) {
  SVGDocument document = ParseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
      <rect width="8" height="8" fill="red" style="display: none"/>
    </svg>
  )svg");

  Renderer renderer;
  renderer.draw(document);

  const RendererBitmap snapshot = renderer.takeSnapshot();
  EXPECT_FALSE(snapshot.empty());
}

TEST(RendererErrorPathsTest, VisibilityHiddenElement) {
  SVGDocument document = ParseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
      <rect width="8" height="8" fill="red" style="visibility: hidden"/>
    </svg>
  )svg");

  Renderer renderer;
  renderer.draw(document);

  const RendererBitmap snapshot = renderer.takeSnapshot();
  EXPECT_FALSE(snapshot.empty());
}

TEST(RendererErrorPathsTest, ZeroOpacityElement) {
  SVGDocument document = ParseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
      <rect width="8" height="8" fill="red" opacity="0"/>
    </svg>
  )svg");

  Renderer renderer;
  renderer.draw(document);

  const RendererBitmap snapshot = renderer.takeSnapshot();
  EXPECT_FALSE(snapshot.empty());
}

TEST(RendererErrorPathsTest, StrokeWithZeroWidth) {
  SVGDocument document = ParseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
      <rect width="6" height="6" x="1" y="1" fill="none" stroke="red" stroke-width="0"/>
    </svg>
  )svg");

  Renderer renderer;
  renderer.draw(document);

  const RendererBitmap snapshot = renderer.takeSnapshot();
  EXPECT_FALSE(snapshot.empty());
}

TEST(RendererErrorPathsTest, FillNone) {
  SVGDocument document = ParseDocument(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
      <rect width="8" height="8" fill="none"/>
    </svg>
  )svg");

  Renderer renderer;
  renderer.draw(document);

  const RendererBitmap snapshot = renderer.takeSnapshot();
  EXPECT_FALSE(snapshot.empty());
}

}  // namespace
}  // namespace donner::svg
