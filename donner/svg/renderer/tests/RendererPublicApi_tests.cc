#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "donner/svg/SVG.h"
#include "donner/svg/renderer/Renderer.h"

namespace donner::svg {
namespace {

SVGDocument ParseDocument(std::string_view svgSource) {
  parser::SVGParser::Options options;
  ParseResult<SVGDocument> maybeDocument = parser::SVGParser::ParseSVG(svgSource, nullptr, options);
  EXPECT_FALSE(maybeDocument.hasError());
  return std::move(maybeDocument.result());
}

RendererBitmap NormalizeSnapshot(RendererBitmap snapshot) {
  const std::size_t tightRowBytes = static_cast<std::size_t>(snapshot.dimensions.x) * 4;
  if (snapshot.rowBytes == tightRowBytes) {
    return snapshot;
  }

  RendererBitmap normalized;
  normalized.dimensions = snapshot.dimensions;
  normalized.rowBytes = tightRowBytes;
  normalized.pixels.resize(tightRowBytes * static_cast<std::size_t>(snapshot.dimensions.y));

  for (int y = 0; y < snapshot.dimensions.y; ++y) {
    std::copy_n(snapshot.pixels.begin() + static_cast<std::size_t>(y) * snapshot.rowBytes,
                tightRowBytes,
                normalized.pixels.begin() + static_cast<std::size_t>(y) * tightRowBytes);
  }

  return normalized;
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

TEST(RendererPublicApiTest, IncrementalStyleInvalidationMatchesFullRender) {
  SVGDocument incrementalDocument = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="12" height="12" viewBox="0 0 12 12">
        <rect id="target" x="2" y="2" width="8" height="8" fill="#ff0000" />
      </svg>
    )svg");

  Renderer incrementalRenderer;
  incrementalRenderer.draw(incrementalDocument);

  auto target = incrementalDocument.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  target->setAttribute("fill", "#0000ff");

  incrementalRenderer.draw(incrementalDocument);
  const RendererBitmap incrementalSnapshot =
      NormalizeSnapshot(incrementalRenderer.takeSnapshot());
  ASSERT_FALSE(incrementalSnapshot.empty());

  SVGDocument fullDocument = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="12" height="12" viewBox="0 0 12 12">
        <rect id="target" x="2" y="2" width="8" height="8" fill="#0000ff" />
      </svg>
    )svg");

  Renderer fullRenderer;
  fullRenderer.draw(fullDocument);
  const RendererBitmap fullSnapshot = NormalizeSnapshot(fullRenderer.takeSnapshot());
  ASSERT_FALSE(fullSnapshot.empty());

  EXPECT_EQ(incrementalSnapshot.dimensions, fullSnapshot.dimensions);
  EXPECT_EQ(incrementalSnapshot.rowBytes, fullSnapshot.rowBytes);
  EXPECT_EQ(incrementalSnapshot.pixels, fullSnapshot.pixels);
}

}  // namespace
}  // namespace donner::svg
