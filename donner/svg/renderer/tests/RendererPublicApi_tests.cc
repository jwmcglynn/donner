#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <string>

#include "donner/svg/SVG.h"
#include "donner/svg/renderer/Renderer.h"

namespace donner::svg {
namespace {

SVGDocument ParseDocument(std::string_view svgSource, bool enableExperimental = false) {
  parser::SVGParser::Options options;
  options.enableExperimental = enableExperimental;
  ParseResult<SVGDocument> maybeDocument = parser::SVGParser::ParseSVG(svgSource, nullptr, options);
  EXPECT_FALSE(maybeDocument.hasError());
  return std::move(maybeDocument.result());
}

TEST(RendererPublicApiTest, DrawProducesSnapshotAndPng) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="8" height="6" viewBox="0 0 8 6">
        <rect width="8" height="6" fill="#00ff00" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);

  const RendererBitmap snapshot = renderer.takeSnapshot();
  ASSERT_FALSE(snapshot.empty());
  EXPECT_EQ(renderer.width(), 8);
  EXPECT_EQ(renderer.height(), 6);
  EXPECT_EQ(snapshot.dimensions, Vector2i(8, 6));
  EXPECT_GT(snapshot.rowBytes, 0u);

  const std::filesystem::path outputPath =
      std::filesystem::path(::testing::TempDir()) / "renderer_public_api.png";
  EXPECT_TRUE(renderer.save(outputPath.c_str()));
  ASSERT_TRUE(std::filesystem::exists(outputPath));
  EXPECT_GT(std::filesystem::file_size(outputPath), 0u);
}

TEST(RendererPublicApiTest, TextUsesDocumentTransformForGlyphPlacement) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="500" height="500" viewBox="0 0 200 200"
           font-size="64">
        <text x="32" y="100">T</text>
      </svg>
    )svg",
                                      /*enableExperimental=*/true);

  Renderer renderer;
  renderer.draw(document);

  const RendererBitmap snapshot = renderer.takeSnapshot();
  ASSERT_FALSE(snapshot.empty());
  ASSERT_EQ(snapshot.dimensions, Vector2i(500, 500));

  const auto pixelAt = [&](int x, int y) -> std::array<uint8_t, 4> {
    const size_t index =
        (static_cast<size_t>(y) * snapshot.rowBytes) + static_cast<size_t>(x) * 4u;
    return {snapshot.pixels[index], snapshot.pixels[index + 1], snapshot.pixels[index + 2],
            snapshot.pixels[index + 3]};
  };

  // The glyph should be placed after the viewBox-to-canvas scale, not at the unscaled SVG-space
  // coordinates near the top-left corner.
  EXPECT_EQ(pixelAt(80, 20)[3], 0);
  EXPECT_GT(pixelAt(120, 150)[3], 0);
}

}  // namespace
}  // namespace donner::svg
