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

// --- Coverage-oriented tests for RenderingContext / RendererTinySkia ---

// Helper: return RGBA at (x,y) from a normalized snapshot.
std::array<uint8_t, 4> PixelAt(const RendererBitmap& snap, int x, int y) {
  const size_t idx =
      static_cast<size_t>(y) * snap.rowBytes + static_cast<size_t>(x) * 4u;
  return {snap.pixels[idx], snap.pixels[idx + 1], snap.pixels[idx + 2], snap.pixels[idx + 3]};
}

// 1. Stroke rendering — rect with stroke-width, stroke-linecap, stroke-linejoin
TEST(RendererPublicApiTest, StrokeRenderingOnRect) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="50" height="50" viewBox="0 0 50 50">
        <rect x="10" y="10" width="30" height="30"
              fill="none" stroke="#ff0000" stroke-width="4"
              stroke-linecap="round" stroke-linejoin="bevel" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  EXPECT_EQ(renderer.width(), 50);
  EXPECT_EQ(renderer.height(), 50);

  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  // The center of the rect should be transparent (fill=none).
  auto center = PixelAt(snapshot, 25, 25);
  EXPECT_EQ(center[3], 0);

  // A point on the stroke (top edge at x=25, y=10) should be red.
  auto strokePx = PixelAt(snapshot, 25, 10);
  EXPECT_GT(strokePx[0], 200);  // Red channel
  EXPECT_GT(strokePx[3], 200);  // Alpha
}

// 2. Stroke-dasharray — verify dashed stroke renders differently from solid
TEST(RendererPublicApiTest, StrokeDasharrayDiffersFromSolid) {
  const char* solidSvg = R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="60" height="20" viewBox="0 0 60 20">
        <line x1="0" y1="10" x2="60" y2="10" stroke="black" stroke-width="4" />
      </svg>
    )svg";
  const char* dashedSvg = R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="60" height="20" viewBox="0 0 60 20">
        <line x1="0" y1="10" x2="60" y2="10" stroke="black" stroke-width="4"
              stroke-dasharray="8 4" />
      </svg>
    )svg";

  Renderer rSolid;
  SVGDocument docSolid = ParseDocument(solidSvg);
  rSolid.draw(docSolid);
  const RendererBitmap snapSolid = NormalizeSnapshot(rSolid.takeSnapshot());

  Renderer rDashed;
  SVGDocument docDashed = ParseDocument(dashedSvg);
  rDashed.draw(docDashed);
  const RendererBitmap snapDashed = NormalizeSnapshot(rDashed.takeSnapshot());

  ASSERT_FALSE(snapSolid.empty());
  ASSERT_FALSE(snapDashed.empty());
  EXPECT_EQ(snapSolid.dimensions, snapDashed.dimensions);
  // Pixels must differ because of the dash gaps.
  EXPECT_NE(snapSolid.pixels, snapDashed.pixels);
}

// 3. Opacity on shape
TEST(RendererPublicApiTest, ShapeOpacity) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="20" height="20" viewBox="0 0 20 20">
        <rect width="20" height="20" fill="#ff0000" opacity="0.5" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  // The pixel should be semi-transparent red (~128 alpha).
  auto px = PixelAt(snapshot, 10, 10);
  EXPECT_GT(px[0], 100);   // Red present
  EXPECT_LT(px[3], 200);   // Not fully opaque
  EXPECT_GT(px[3], 50);    // Not fully transparent
}

// 4. Fill-opacity + stroke-opacity
TEST(RendererPublicApiTest, FillOpacityAndStrokeOpacity) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="40" height="40" viewBox="0 0 40 40">
        <rect x="5" y="5" width="30" height="30"
              fill="#00ff00" fill-opacity="0.3"
              stroke="#ff0000" stroke-opacity="0.8" stroke-width="4" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  // Interior should have semi-transparent green fill.
  auto interior = PixelAt(snapshot, 20, 20);
  EXPECT_GT(interior[1], 30);   // Some green
  EXPECT_LT(interior[3], 130);  // Low alpha from fill-opacity 0.3

  // Stroke edge should have higher alpha from stroke-opacity 0.8.
  auto strokeEdge = PixelAt(snapshot, 5, 20);
  EXPECT_GT(strokeEdge[3], interior[3]);
}

// 5. Marker rendering — path with marker-start, marker-mid, marker-end
TEST(RendererPublicApiTest, MarkerRendering) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="80" height="40" viewBox="0 0 80 40">
        <defs>
          <marker id="dot" viewBox="0 0 10 10" refX="5" refY="5"
                  markerWidth="6" markerHeight="6" markerUnits="strokeWidth">
            <circle cx="5" cy="5" r="5" fill="red" />
          </marker>
        </defs>
        <polyline points="10,20 40,20 70,20" fill="none" stroke="black" stroke-width="2"
                  marker-start="url(#dot)" marker-mid="url(#dot)" marker-end="url(#dot)" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  EXPECT_EQ(renderer.width(), 80);
  EXPECT_EQ(renderer.height(), 40);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  // Marker-start position (near x=10, y=20) should have red marker pixels.
  auto markerStart = PixelAt(snapshot, 10, 20);
  EXPECT_GT(markerStart[3], 0);  // Not transparent — marker drawn here
}

// 6. Use element rendering
TEST(RendererPublicApiTest, UseElementRendering) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="40" height="20" viewBox="0 0 40 20">
        <defs>
          <rect id="box" width="10" height="10" fill="#0000ff" />
        </defs>
        <use href="#box" x="5" y="5" />
        <use href="#box" x="25" y="5" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  // Both use instances should produce blue pixels.
  auto first = PixelAt(snapshot, 10, 10);
  EXPECT_GT(first[2], 200);  // Blue
  EXPECT_GT(first[3], 200);  // Opaque

  auto second = PixelAt(snapshot, 30, 10);
  EXPECT_GT(second[2], 200);
  EXPECT_GT(second[3], 200);
}

// 7. Nested group transforms
TEST(RendererPublicApiTest, NestedGroupTransforms) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="60" height="60" viewBox="0 0 60 60">
        <g transform="translate(10,10)">
          <g transform="scale(2)">
            <rect width="10" height="10" fill="#ff0000" />
          </g>
        </g>
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  // The rect should be 20x20 at position (10,10) due to translate + scale(2).
  // Inside the transformed rect (e.g., 20, 20) should be red.
  auto inside = PixelAt(snapshot, 20, 20);
  EXPECT_GT(inside[0], 200);
  EXPECT_GT(inside[3], 200);

  // Outside the transformed rect (e.g., 5, 5) should be transparent.
  auto outside = PixelAt(snapshot, 5, 5);
  EXPECT_EQ(outside[3], 0);

  // Beyond the transformed rect (e.g., 35, 35) should be transparent.
  auto beyond = PixelAt(snapshot, 35, 35);
  EXPECT_EQ(beyond[3], 0);
}

// 8. viewBox scaling
TEST(RendererPublicApiTest, ViewBoxScaling) {
  // viewBox is 100x100 but output size is 50x50 — everything scales by 0.5.
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="50" height="50" viewBox="0 0 100 100">
        <rect width="100" height="100" fill="#00ff00" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  EXPECT_EQ(renderer.width(), 50);
  EXPECT_EQ(renderer.height(), 50);

  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  // The green rect should fill the entire 50x50 canvas.
  auto corner = PixelAt(snapshot, 0, 0);
  EXPECT_GT(corner[1], 200);
  EXPECT_GT(corner[3], 200);

  auto center = PixelAt(snapshot, 25, 25);
  EXPECT_GT(center[1], 200);
  EXPECT_GT(center[3], 200);
}

// 9. Symbol with use
TEST(RendererPublicApiTest, SymbolWithUse) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="40" height="40" viewBox="0 0 40 40">
        <defs>
          <symbol id="sym" viewBox="0 0 10 10">
            <rect width="10" height="10" fill="#ff00ff" />
          </symbol>
        </defs>
        <use href="#sym" x="5" y="5" width="20" height="20" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  // The symbol should be rendered at (5,5) with 20x20 size, containing magenta.
  auto inside = PixelAt(snapshot, 15, 15);
  EXPECT_GT(inside[0], 200);  // Red channel of magenta
  EXPECT_GT(inside[2], 200);  // Blue channel of magenta
  EXPECT_GT(inside[3], 200);  // Opaque

  // Outside the use area should be transparent.
  auto outside = PixelAt(snapshot, 2, 2);
  EXPECT_EQ(outside[3], 0);
}

// 10. CSS class styling
TEST(RendererPublicApiTest, CssClassStyling) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="20" height="20" viewBox="0 0 20 20">
        <style>.foo { fill: red }</style>
        <rect class="foo" width="20" height="20" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  auto px = PixelAt(snapshot, 10, 10);
  EXPECT_GT(px[0], 200);  // Red
  EXPECT_LT(px[1], 50);   // Not green
  EXPECT_LT(px[2], 50);   // Not blue
  EXPECT_GT(px[3], 200);  // Opaque
}

// 11. Inline style override
TEST(RendererPublicApiTest, InlineStyleOverridesAttribute) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="20" height="20" viewBox="0 0 20 20">
        <rect fill="blue" style="fill: red" width="20" height="20" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  // Style should override the fill attribute — result should be red, not blue.
  auto px = PixelAt(snapshot, 10, 10);
  EXPECT_GT(px[0], 200);  // Red
  EXPECT_LT(px[2], 50);   // Not blue
  EXPECT_GT(px[3], 200);
}

// 12. Multiple gradients — two shapes with different linear gradient fills
TEST(RendererPublicApiTest, MultipleLinearGradients) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="40" height="20" viewBox="0 0 40 20">
        <defs>
          <linearGradient id="g1" x1="0" y1="0" x2="1" y2="0">
            <stop offset="0" stop-color="red" />
            <stop offset="1" stop-color="blue" />
          </linearGradient>
          <linearGradient id="g2" x1="0" y1="0" x2="1" y2="0">
            <stop offset="0" stop-color="green" />
            <stop offset="1" stop-color="yellow" />
          </linearGradient>
        </defs>
        <rect x="0" y="0" width="20" height="20" fill="url(#g1)" />
        <rect x="20" y="0" width="20" height="20" fill="url(#g2)" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  // Left rect's left edge should be reddish.
  auto leftEdge = PixelAt(snapshot, 1, 10);
  EXPECT_GT(leftEdge[0], 200);  // Red
  EXPECT_GT(leftEdge[3], 200);

  // Right rect's left edge should be greenish.
  auto rightEdge = PixelAt(snapshot, 21, 10);
  EXPECT_GT(rightEdge[1], 100);  // Green
  EXPECT_GT(rightEdge[3], 200);
}

// 13. Radial gradient
TEST(RendererPublicApiTest, RadialGradient) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="40" height="40" viewBox="0 0 40 40">
        <defs>
          <radialGradient id="rg" cx="0.5" cy="0.5" r="0.5">
            <stop offset="0" stop-color="white" />
            <stop offset="1" stop-color="black" />
          </radialGradient>
        </defs>
        <rect width="40" height="40" fill="url(#rg)" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  // Center should be white (near 255).
  auto center = PixelAt(snapshot, 20, 20);
  EXPECT_GT(center[0], 200);
  EXPECT_GT(center[1], 200);
  EXPECT_GT(center[2], 200);
  EXPECT_GT(center[3], 200);

  // Corner should be darker.
  auto corner = PixelAt(snapshot, 0, 0);
  EXPECT_LT(corner[0], center[0]);
}

// 14. Image element — <image> with data: URI (small inline 2x2 red PNG)
// Exercises the image loading and drawImage code paths in RenderingContext/RendererTinySkia.
TEST(RendererPublicApiTest, ImageElementDataUri) {
  // A 2x2 solid red PNG encoded as base64 data URI.
  // Generated with: python3 -c "import base64,struct,zlib; ..."
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg"
           xmlns:xlink="http://www.w3.org/1999/xlink"
           width="20" height="20" viewBox="0 0 20 20">
        <image x="0" y="0" width="20" height="20"
               href="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAEUlEQVR42mP4z8DwH4QZYAwAR8oH+Rq28akAAAAASUVORK5CYII=" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());
  EXPECT_EQ(snapshot.dimensions, Vector2i(20, 20));

  // The data URI image should be decoded and rendered.  The 2x2 red PNG is scaled
  // to fill the 20x20 viewport, so center pixel should be red.
  auto px = PixelAt(snapshot, 10, 10);
  EXPECT_GT(px[0], 200);  // Red
  EXPECT_GT(px[3], 200);  // Opaque
}

// 15. Empty document — render SVG with no visible content
TEST(RendererPublicApiTest, EmptyDocumentProducesValidOutput) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="30" height="30" viewBox="0 0 30 30">
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  EXPECT_EQ(renderer.width(), 30);
  EXPECT_EQ(renderer.height(), 30);

  const RendererBitmap snapshot = renderer.takeSnapshot();
  ASSERT_FALSE(snapshot.empty());
  EXPECT_EQ(snapshot.dimensions, Vector2i(30, 30));

  // All pixels should be transparent.
  auto px = PixelAt(NormalizeSnapshot(renderer.takeSnapshot()), 15, 15);
  EXPECT_EQ(px[3], 0);
}

// 16. Circle element exercises drawEllipse path
TEST(RendererPublicApiTest, CircleElement) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="40" height="40" viewBox="0 0 40 40">
        <circle cx="20" cy="20" r="15" fill="#00ff00" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  // Center should be green.
  auto center = PixelAt(snapshot, 20, 20);
  EXPECT_GT(center[1], 200);
  EXPECT_GT(center[3], 200);

  // Corner should be transparent (outside circle).
  auto corner = PixelAt(snapshot, 0, 0);
  EXPECT_EQ(corner[3], 0);
}

// 17. Ellipse element exercises drawEllipse code path with separate rx/ry
TEST(RendererPublicApiTest, EllipseElement) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="60" height="30" viewBox="0 0 60 30">
        <ellipse cx="30" cy="15" rx="25" ry="10" fill="#0000ff" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  // Center should be blue.
  auto center = PixelAt(snapshot, 30, 15);
  EXPECT_GT(center[2], 200);
  EXPECT_GT(center[3], 200);
}

// 18. Pattern fill
TEST(RendererPublicApiTest, PatternFill) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="40" height="40" viewBox="0 0 40 40">
        <defs>
          <pattern id="pat" width="10" height="10" patternUnits="userSpaceOnUse">
            <rect width="5" height="5" fill="red" />
            <rect x="5" y="5" width="5" height="5" fill="blue" />
          </pattern>
        </defs>
        <rect width="40" height="40" fill="url(#pat)" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  // The pattern should produce non-transparent content.
  auto px = PixelAt(snapshot, 2, 2);
  EXPECT_GT(px[3], 0);  // Pattern tile drawn

  // Different tile positions should potentially have different colors (red vs blue).
  auto px2 = PixelAt(snapshot, 7, 7);
  EXPECT_GT(px2[3], 0);
}

// 19. ClipPath
TEST(RendererPublicApiTest, ClipPathCropsContent) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="40" height="40" viewBox="0 0 40 40">
        <defs>
          <clipPath id="clip">
            <rect x="10" y="10" width="20" height="20" />
          </clipPath>
        </defs>
        <rect width="40" height="40" fill="red" clip-path="url(#clip)" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  // Inside clip region should be red.
  auto inside = PixelAt(snapshot, 20, 20);
  EXPECT_GT(inside[0], 200);
  EXPECT_GT(inside[3], 200);

  // Outside clip region should be transparent.
  auto outside = PixelAt(snapshot, 2, 2);
  EXPECT_EQ(outside[3], 0);
}

// 20. Mask element
TEST(RendererPublicApiTest, MaskElement) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="40" height="40" viewBox="0 0 40 40">
        <defs>
          <mask id="m">
            <rect width="40" height="40" fill="white" />
            <rect x="10" y="10" width="20" height="20" fill="black" />
          </mask>
        </defs>
        <rect width="40" height="40" fill="green" mask="url(#m)" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  // Outside the black hole (corner) — mask is white, so green should show.
  auto corner = PixelAt(snapshot, 2, 2);
  EXPECT_GT(corner[1], 100);  // Green
  EXPECT_GT(corner[3], 100);

  // Inside the black hole (center) — mask is black, so content should be masked.
  auto center = PixelAt(snapshot, 20, 20);
  EXPECT_LT(center[3], corner[3]);  // More transparent than the corner
}

// 21. Mix-blend-mode triggers isolated layer
TEST(RendererPublicApiTest, MixBlendModeIsolatedLayer) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="30" height="30" viewBox="0 0 30 30">
        <rect width="30" height="30" fill="white" />
        <rect width="30" height="30" fill="red" style="mix-blend-mode: multiply" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  // multiply(white, red) = red
  auto px = PixelAt(snapshot, 15, 15);
  EXPECT_GT(px[0], 200);  // Red channel
  EXPECT_GT(px[3], 200);  // Opaque
}

// 22. Visibility hidden — element should not appear
TEST(RendererPublicApiTest, VisibilityHidden) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="20" height="20" viewBox="0 0 20 20">
        <rect width="20" height="20" fill="red" visibility="hidden" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  // Hidden element should not render — pixel should be transparent.
  auto px = PixelAt(snapshot, 10, 10);
  EXPECT_EQ(px[3], 0);
}

// 23. Display none — element and children should not appear
TEST(RendererPublicApiTest, DisplayNone) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="20" height="20" viewBox="0 0 20 20">
        <g display="none">
          <rect width="20" height="20" fill="blue" />
        </g>
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  auto px = PixelAt(snapshot, 10, 10);
  EXPECT_EQ(px[3], 0);
}

// 24. Gradient with gradient-transform exercises resolveGradientTransform
TEST(RendererPublicApiTest, GradientWithTransform) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="40" height="40" viewBox="0 0 40 40">
        <defs>
          <linearGradient id="g" x1="0" y1="0" x2="1" y2="0"
                          gradientTransform="rotate(90 0.5 0.5)">
            <stop offset="0" stop-color="red" />
            <stop offset="1" stop-color="blue" />
          </linearGradient>
        </defs>
        <rect width="40" height="40" fill="url(#g)" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  // Because of 90-degree rotation, the gradient should go top to bottom.
  auto top = PixelAt(snapshot, 20, 1);
  auto bottom = PixelAt(snapshot, 20, 38);
  // Top should be more red, bottom more blue.
  EXPECT_GT(top[0], bottom[0]);   // More red at top
  EXPECT_LT(top[2], bottom[2]);   // Less blue at top
}

// 25. Path element (exercises drawPath code path)
TEST(RendererPublicApiTest, PathElement) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="40" height="40" viewBox="0 0 40 40">
        <path d="M5,5 L35,5 L35,35 L5,35 Z" fill="#ff8800" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  // Inside the path should be orange.
  auto inside = PixelAt(snapshot, 20, 20);
  EXPECT_GT(inside[0], 200);  // Red component of orange
  EXPECT_GT(inside[1], 100);  // Green component of orange
  EXPECT_GT(inside[3], 200);
}

// 26. Gradient userSpaceOnUse exercises non-objectBoundingBox branch
TEST(RendererPublicApiTest, GradientUserSpaceOnUse) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="40" height="40" viewBox="0 0 40 40">
        <defs>
          <linearGradient id="gu" x1="0" y1="0" x2="40" y2="0"
                          gradientUnits="userSpaceOnUse">
            <stop offset="0" stop-color="cyan" />
            <stop offset="1" stop-color="magenta" />
          </linearGradient>
        </defs>
        <rect width="40" height="40" fill="url(#gu)" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  // Left edge should be cyan (R=0, G=255, B=255).
  auto left = PixelAt(snapshot, 1, 20);
  EXPECT_LT(left[0], 50);    // Low red (cyan)
  EXPECT_GT(left[1], 200);   // High green
  EXPECT_GT(left[2], 200);   // High blue

  // Right edge should be magenta (R=255, G=0, B=255).
  auto right = PixelAt(snapshot, 38, 20);
  EXPECT_GT(right[0], 200);  // High red (magenta)
  EXPECT_GT(right[2], 200);  // High blue
}

// 27. Stroke with path (exercises stroke on drawPath, not drawRect)
TEST(RendererPublicApiTest, StrokeOnPath) {
  SVGDocument document = ParseDocument(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="50" height="50" viewBox="0 0 50 50">
        <path d="M10,25 L40,25" fill="none" stroke="#ff0000" stroke-width="6"
              stroke-linecap="square" />
      </svg>
    )svg");

  Renderer renderer;
  renderer.draw(document);
  const RendererBitmap snapshot = NormalizeSnapshot(renderer.takeSnapshot());
  ASSERT_FALSE(snapshot.empty());

  // On the line at y=25 should be red.
  auto onLine = PixelAt(snapshot, 25, 25);
  EXPECT_GT(onLine[0], 200);
  EXPECT_GT(onLine[3], 200);

  // Far from the line should be transparent.
  auto farAway = PixelAt(snapshot, 25, 5);
  EXPECT_EQ(farAway[3], 0);
}

}  // namespace
}  // namespace donner::svg
