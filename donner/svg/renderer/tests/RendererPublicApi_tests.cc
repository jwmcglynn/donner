#include <gtest/gtest.h>

#include <filesystem>
#include <string>

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

}  // namespace
}  // namespace donner::svg
