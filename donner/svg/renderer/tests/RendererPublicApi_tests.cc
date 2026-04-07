#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include "donner/svg/SVG.h"
#include "donner/svg/SVGRectElement.h"
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

// --- Dirty flag fast path tests ---

TEST(RendererPublicApiTest, DoubleDrawWithoutMutationProducesSameOutput) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="10" height="10" viewBox="0 0 10 10">
        <rect width="10" height="10" fill="#ff0000" />
        <circle cx="5" cy="5" r="3" fill="#00ff00" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap firstSnapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(firstSnapshot.empty());

  // Second draw without any DOM mutation — exercises the dirty-flag fast path in
  // RenderingContext::instantiateRenderTree().
  renderer.draw(document);
  const RendererBitmap secondSnapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(secondSnapshot.empty());

  EXPECT_EQ(firstSnapshot.dimensions, secondSnapshot.dimensions);
  EXPECT_EQ(firstSnapshot.pixels, secondSnapshot.pixels);
}

TEST(RendererPublicApiTest, TripleDrawWithoutMutationStaysStable) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="8" height="8" viewBox="0 0 8 8">
        <rect x="1" y="1" width="6" height="6" fill="#abcdef" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snap1 = NormalizeSnapshot(renderer.takeSnapshot());

  renderer.draw(document);
  renderer.draw(document);
  const RendererBitmap snap3 = NormalizeSnapshot(renderer.takeSnapshot());

  EXPECT_EQ(snap1.pixels, snap3.pixels);
}

// --- DOM mutation invalidation tests ---

TEST(RendererPublicApiTest, ChangeFillColorInvalidatesAndProducesNewOutput) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="8" height="8" viewBox="0 0 8 8">
        <rect id="r" width="8" height="8" fill="#ff0000" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap before = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(before.empty());

  auto elem = document.querySelector("#r");
  ASSERT_TRUE(elem.has_value());
  elem->setAttribute("fill", "#0000ff");

  renderer.draw(document);
  const RendererBitmap after = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(after.empty());

  EXPECT_EQ(before.dimensions, after.dimensions);
  // Pixels must differ because fill changed from red to blue.
  EXPECT_NE(before.pixels, after.pixels);
}

TEST(RendererPublicApiTest, ChangeOpacityAttributeInvalidates) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="8" height="8" viewBox="0 0 8 8">
        <rect id="r" width="8" height="8" fill="#ff0000" opacity="1" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap before = NormalizeSnapshot(renderer.takeSnapshot());

  // Change opacity — a presentation attribute that should invalidate correctly.
  auto elem = document.querySelector("#r");
  ASSERT_TRUE(elem.has_value());
  elem->setAttribute("opacity", "0.5");

  renderer.draw(document);
  const RendererBitmap after = NormalizeSnapshot(renderer.takeSnapshot());

  EXPECT_EQ(before.dimensions, after.dimensions);
  EXPECT_NE(before.pixels, after.pixels);
}

TEST(RendererPublicApiTest, MutationThenNoMutationUsesCache) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="8" height="8" viewBox="0 0 8 8">
        <rect id="r" width="8" height="8" fill="#ff0000" />
      </svg>
    )svg");

  Renderer renderer;

  // Initial render.
  renderer.draw(document);

  // Mutate and render.
  auto elem = document.querySelector("#r");
  ASSERT_TRUE(elem.has_value());
  elem->setAttribute("fill", "#00ff00");
  renderer.draw(document);
  const RendererBitmap mutated = NormalizeSnapshot(renderer.takeSnapshot());

  // Render again without mutation — should use the fast path and produce identical output.
  renderer.draw(document);
  const RendererBitmap cached = NormalizeSnapshot(renderer.takeSnapshot());

  EXPECT_EQ(mutated.pixels, cached.pixels);
}

// --- Style invalidation tests ---

TEST(RendererPublicApiTest, SetStyleAttributeInvalidates) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="8" height="8" viewBox="0 0 8 8">
        <rect id="r" width="8" height="8" fill="#ff0000" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap before = NormalizeSnapshot(renderer.takeSnapshot());

  auto elem = document.querySelector("#r");
  ASSERT_TRUE(elem.has_value());
  elem->setStyle("fill: blue");

  renderer.draw(document);
  const RendererBitmap after = NormalizeSnapshot(renderer.takeSnapshot());

  EXPECT_EQ(before.dimensions, after.dimensions);
  EXPECT_NE(before.pixels, after.pixels);
}

TEST(RendererPublicApiTest, SetStyleAttributeMatchesFullRender) {
  // Render doc1 with red, mutate to blue via setStyle, compare against doc2 parsed with blue.
  SVGDocument doc1 = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="8" height="8" viewBox="0 0 8 8">
        <rect id="r" width="8" height="8" fill="#ff0000" />
      </svg>
    )svg");

  Renderer r1;
  r1.draw(doc1);

  auto elem = doc1.querySelector("#r");
  ASSERT_TRUE(elem.has_value());
  elem->setStyle("fill: #0000ff");
  r1.draw(doc1);
  const RendererBitmap incremental = NormalizeSnapshot(r1.takeSnapshot());

  SVGDocument doc2 = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="8" height="8" viewBox="0 0 8 8">
        <rect width="8" height="8" style="fill: #0000ff" />
      </svg>
    )svg");

  Renderer r2;
  r2.draw(doc2);
  const RendererBitmap full = NormalizeSnapshot(r2.takeSnapshot());

  EXPECT_EQ(incremental.dimensions, full.dimensions);
  EXPECT_EQ(incremental.pixels, full.pixels);
}

// --- Tree mutation tests ---

TEST(RendererPublicApiTest, AppendChildInvalidates) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="10" height="10" viewBox="0 0 10 10">
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap before = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(before.empty());

  // Add a filled rect that covers the entire canvas.
  SVGRectElement rect = SVGRectElement::Create(document);
  rect.setAttribute("width", "10");
  rect.setAttribute("height", "10");
  rect.setAttribute("fill", "#ff0000");
  document.svgElement().appendChild(rect);

  renderer.draw(document);
  const RendererBitmap after = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(after.empty());

  EXPECT_EQ(before.dimensions, after.dimensions);
  EXPECT_NE(before.pixels, after.pixels);
}

TEST(RendererPublicApiTest, RemoveChildInvalidates) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="10" height="10" viewBox="0 0 10 10">
        <rect id="r" width="10" height="10" fill="#ff0000" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap before = NormalizeSnapshot(renderer.takeSnapshot());

  auto elem = document.querySelector("#r");
  ASSERT_TRUE(elem.has_value());
  document.svgElement().removeChild(*elem);

  renderer.draw(document);
  const RendererBitmap after = NormalizeSnapshot(renderer.takeSnapshot());

  EXPECT_EQ(before.dimensions, after.dimensions);
  // After removing the red rect, the canvas should be transparent/white (different from before).
  EXPECT_NE(before.pixels, after.pixels);
}

TEST(RendererPublicApiTest, AppendChildMatchesFullRender) {
  // Start with empty SVG, add a green rect, verify the result matches a document parsed with the
  // rect already present.
  SVGDocument doc1 = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="10" height="10" viewBox="0 0 10 10">
      </svg>
    )svg");

  Renderer r1;
  r1.draw(doc1);

  SVGRectElement rect = SVGRectElement::Create(doc1);
  rect.setAttribute("width", "10");
  rect.setAttribute("height", "10");
  rect.setAttribute("fill", "#00ff00");
  doc1.svgElement().appendChild(rect);

  r1.draw(doc1);
  const RendererBitmap incremental = NormalizeSnapshot(r1.takeSnapshot());

  SVGDocument doc2 = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="10" height="10" viewBox="0 0 10 10">
        <rect width="10" height="10" fill="#00ff00" />
      </svg>
    )svg");

  Renderer r2;
  r2.draw(doc2);
  const RendererBitmap full = NormalizeSnapshot(r2.takeSnapshot());

  EXPECT_EQ(incremental.dimensions, full.dimensions);
  EXPECT_EQ(incremental.pixels, full.pixels);
}

TEST(RendererPublicApiTest, MultipleSequentialMutationsAccumulate) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="10" height="10" viewBox="0 0 10 10">
        <rect id="r" width="10" height="10" fill="#ff0000" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap initial = NormalizeSnapshot(renderer.takeSnapshot());

  // First mutation: change fill color.
  auto elem = document.querySelector("#r");
  ASSERT_TRUE(elem.has_value());
  elem->setAttribute("fill", "#00ff00");
  renderer.draw(document);
  const RendererBitmap afterFill = NormalizeSnapshot(renderer.takeSnapshot());

  // Second mutation: change opacity.
  elem->setAttribute("opacity", "0.5");
  renderer.draw(document);
  const RendererBitmap afterOpacity = NormalizeSnapshot(renderer.takeSnapshot());

  // Third render with no mutation — fast path.
  renderer.draw(document);
  const RendererBitmap cached = NormalizeSnapshot(renderer.takeSnapshot());

  // Each mutation should produce a different result from the previous.
  EXPECT_NE(initial.pixels, afterFill.pixels);
  EXPECT_NE(afterFill.pixels, afterOpacity.pixels);
  // No-mutation render should match the previous.
  EXPECT_EQ(afterOpacity.pixels, cached.pixels);
}

}  // namespace
}  // namespace donner::svg
