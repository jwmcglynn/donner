#include "donner/editor/OverlayRenderer.h"

#include <gmock/gmock.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <ostream>
#include <span>
#include <string>
#include <string_view>

#include "donner/base/Transform.h"
#include "donner/editor/EditorApp.h"
#include "donner/svg/DocumentState.h"
#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/tests/RgbaTestMatchers.h"
#include "gtest/gtest.h"

namespace donner::editor {

void PrintTo(const Vector2d& vector, std::ostream* os) {
  *os << "Vector2d{x=" << vector.x << ", y=" << vector.y << "}";
}

namespace {

using ::testing::_;
using ::testing::AllOf;
using ::testing::DoubleNear;
using ::testing::ElementsAre;
using ::testing::Field;

constexpr double kPi = 3.14159265358979323846;

constexpr std::string_view kTrivialSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">
         <rect id="r1" x="20" y="30" width="40" height="50" fill="red"/>
       </svg>)";

bool AnyBoxContains(std::span<const Box2d> boxes, const Vector2d& point) {
  return std::ranges::any_of(boxes, [&](const Box2d& box) { return box.contains(point); });
}

auto Vector2dNear(Vector2d expected, double tolerance) {
  return AllOf(Field("x", &Vector2d::x, DoubleNear(expected.x, tolerance)),
               Field("y", &Vector2d::y, DoubleNear(expected.y, tolerance)));
}

auto PathControlLineIs(Vector2d anchorDoc, Vector2d controlDoc) {
  return AllOf(Field("anchorDoc", &SelectionChromeSnapshot::PathControlLine::anchorDoc,
                     Vector2dNear(anchorDoc, 1e-9)),
               Field("controlDoc", &SelectionChromeSnapshot::PathControlLine::controlDoc,
                     Vector2dNear(controlDoc, 1e-9)));
}

Path LinePath(Vector2d start, Vector2d end) {
  PathBuilder builder;
  builder.moveTo(start);
  builder.lineTo(end);
  return builder.build();
}

Path RectPath(double x, double y, double width, double height) {
  PathBuilder builder;
  builder.moveTo(Vector2d(x, y));
  builder.lineTo(Vector2d(x + width, y));
  builder.lineTo(Vector2d(x + width, y + height));
  builder.lineTo(Vector2d(x, y + height));
  builder.closePath();
  return builder.build();
}

auto DrawSnapshot(const SelectionChromeSnapshot& snapshot) {
  svg::Renderer renderer;
  svg::RenderViewport viewport;
  viewport.size = Vector2d(120.0, 120.0);
  viewport.devicePixelRatio = 1.0;
  renderer.beginFrame(viewport);
  OverlayRenderer::drawChromeFromSnapshot(renderer, snapshot);
  renderer.endFrame();
  return renderer.takeSnapshot();
}

bool HasAnyNonTransparentPixel(const svg::RendererBitmap& bitmap) {
  for (std::size_t y = 0; y < static_cast<std::size_t>(bitmap.dimensions.y); ++y) {
    const std::uint8_t* row = bitmap.pixels.data() + y * bitmap.rowBytes;
    for (std::size_t x = 0; x < static_cast<std::size_t>(bitmap.dimensions.x); ++x) {
      if (row[x * 4 + 3] != 0) {
        return true;
      }
    }
  }
  return false;
}

SelectionChromeSnapshot CaptureLivePathPreview(const svg::SVGElement& element) {
  return OverlayRenderer::captureChromeSnapshot(
      std::span<const svg::SVGElement>(), std::nullopt, Transform2d(), std::nullopt,
      std::span<const svg::SVGElement>(), std::nullopt, SelectionChromeDetail::Full, Transform2d(),
      std::nullopt, 1.0, std::optional<svg::SVGElement>(element));
}

MATCHER(DimmedGrayBlueStrokePixel,
        "a dimmed gray-blue selection-chrome stroke pixel with nonzero red") {
  const std::array<std::uint8_t, 4> pixel = {arg[0], arg[1], arg[2], arg[3]};
  *result_listener << "actual RGBA=" << svg::test::FormatRgba(pixel);

  return pixel[0] > 0 && pixel[1] < 0xc8 && pixel[2] < 0xff && pixel[2] > pixel[0] &&
         pixel[1] > pixel[0];
}

// OverlayRenderer is hard to unit-test in isolation because the canvas
// primitives end up in a renderer-owned frame buffer that we don't read
// back at the unit-test layer. The test plan in `editor.md` calls these
// out as belonging to the framebuffer-golden tier (M4).
//
// What we *can* unit-test cheaply: that calling `drawChrome` does not
// crash on any combination of (no document / document but no selection /
// document with valid selection / document with stale selection
// referencing a since-deleted entity). The full visual verification
// happens in the example viewer and in the future M4 framebuffer
// golden tests.

TEST(OverlayRendererTest, NoOpWithoutDocument) {
  EditorApp app;
  svg::Renderer renderer;
  // No document loaded → drawChrome must early-out before touching the
  // renderer. The test passes if this doesn't crash.
  OverlayRenderer::drawChrome(renderer, app);
  SUCCEED();
}

TEST(OverlayRendererTest, NoOpWithoutSelection) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  svg::Renderer renderer;
  renderer.draw(app.document().document());
  // Selection is empty by default → drawChrome should be a no-op.
  OverlayRenderer::drawChrome(renderer, app);
  SUCCEED();
}

TEST(OverlayRendererTest, EmitsChromeForSelectedElement) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  auto rect = app.document().document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());
  app.setSelection(*rect);

  svg::Renderer renderer;
  renderer.draw(app.document().document());
  OverlayRenderer::drawChrome(renderer, app);

  // The renderer should have produced a non-empty bitmap that includes
  // both the document fill and the chrome stroke. We can't easily diff
  // pixels here without going to the framebuffer-golden tier, but we
  // can at least confirm the renderer produced *something*.
  const auto bitmap = renderer.takeSnapshot();
  EXPECT_FALSE(bitmap.empty());
  EXPECT_GT(bitmap.dimensions.x, 0);
  EXPECT_GT(bitmap.dimensions.y, 0);
}

TEST(OverlayRendererTest, CaptureSnapshotIncludesSourceHoverChromeWithoutSelection) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  auto rect = app.document().document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());

  const std::array<svg::SVGElement, 1> hoverElements{*rect};
  const SelectionChromeSnapshot snapshot = OverlayRenderer::captureChromeSnapshot(
      std::span<const svg::SVGElement>(), std::nullopt, Transform2d(), std::nullopt,
      std::span<const svg::SVGElement>(hoverElements));

  EXPECT_TRUE(snapshot.paths.empty());
  EXPECT_TRUE(snapshot.aabbsDoc.empty());
  EXPECT_TRUE(snapshot.handleBoxesDoc.empty());
  EXPECT_FALSE(snapshot.hoverPaths.empty());
  EXPECT_FALSE(snapshot.hoverAabbsDoc.empty());
}

TEST(OverlayRendererTest, CaptureSnapshotAllowsConcurrentDomSelectionAndHover) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));
  app.document().document().setThreadingMode(svg::ThreadingMode::ConcurrentDom);

  auto rect = app.document().document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());
  app.setSelection(*rect);

  const std::array<svg::SVGElement, 1> hoverElements{*rect};
  const SelectionChromeSnapshot snapshot = OverlayRenderer::captureChromeSnapshot(
      std::span<const svg::SVGElement>(app.selectedElements()), std::nullopt, Transform2d(),
      std::nullopt, std::span<const svg::SVGElement>(hoverElements));

  EXPECT_FALSE(snapshot.paths.empty());
  EXPECT_FALSE(snapshot.aabbsDoc.empty());
  EXPECT_FALSE(snapshot.hoverPaths.empty());
  EXPECT_FALSE(snapshot.hoverAabbsDoc.empty());
}

TEST(OverlayRendererTest, PathOutlinesOnlyOmitsSelectionBoundsAndHandles) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  auto rect = app.document().document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());
  app.setSelection(*rect);

  const SelectionChromeSnapshot snapshot = OverlayRenderer::captureChromeSnapshot(
      std::span<const svg::SVGElement>(app.selectedElements()), std::nullopt, Transform2d(),
      std::nullopt, std::span<const svg::SVGElement>(), std::nullopt,
      SelectionChromeDetail::PathOutlinesOnly);

  EXPECT_FALSE(snapshot.paths.empty());
  EXPECT_TRUE(snapshot.aabbsDoc.empty());
  EXPECT_FALSE(snapshot.orientedBoundsDoc.has_value());
  EXPECT_TRUE(snapshot.handleBoxesDoc.empty());
}

TEST(OverlayRendererTest, SelectedPathSnapshotIncludesAnchorsAndControlLines) {
  constexpr std::string_view kPathSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
              <path id="curve" d="M 10 20 C 20 10 40 10 50 20 L 80 20"
                    fill="none" stroke="black"/>
            </svg>)svg";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kPathSvg));

  auto path = app.document().document().querySelector("#curve");
  ASSERT_TRUE(path.has_value());
  app.setSelection(*path);

  const SelectionChromeSnapshot snapshot = OverlayRenderer::captureChromeSnapshot(
      std::span<const svg::SVGElement>(app.selectedElements()), std::nullopt, Transform2d(),
      std::nullopt, std::span<const svg::SVGElement>(), std::nullopt,
      SelectionChromeDetail::PathOutlinesOnly);

  EXPECT_EQ(snapshot.paths.size(), 1u);
  EXPECT_EQ(snapshot.pathAnchorBoxesDoc.size(), 3u);
  EXPECT_EQ(snapshot.pathControlLinesDoc.size(), 2u);
  EXPECT_EQ(snapshot.pathControlPointBoxesDoc.size(), 2u);
  EXPECT_TRUE(snapshot.aabbsDoc.empty());
  EXPECT_TRUE(snapshot.handleBoxesDoc.empty());

  EXPECT_TRUE(AnyBoxContains(snapshot.pathAnchorBoxesDoc, Vector2d(10.0, 20.0)));
  EXPECT_TRUE(AnyBoxContains(snapshot.pathAnchorBoxesDoc, Vector2d(50.0, 20.0)));
  EXPECT_TRUE(AnyBoxContains(snapshot.pathAnchorBoxesDoc, Vector2d(80.0, 20.0)));
  EXPECT_TRUE(AnyBoxContains(snapshot.pathControlPointBoxesDoc, Vector2d(20.0, 10.0)));
  EXPECT_TRUE(AnyBoxContains(snapshot.pathControlPointBoxesDoc, Vector2d(40.0, 10.0)));

  EXPECT_THAT(snapshot.pathControlLinesDoc,
              ElementsAre(PathControlLineIs(Vector2d(10.0, 20.0), Vector2d(20.0, 10.0)),
                          PathControlLineIs(Vector2d(50.0, 20.0), Vector2d(40.0, 10.0))));
}

TEST(OverlayRendererTest, FullSelectionChromeOmitsPathPointChrome) {
  constexpr std::string_view kPathSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
              <path id="curve" d="M 10 20 C 20 10 40 10 50 20"
                    fill="none" stroke="black"/>
            </svg>)svg";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kPathSvg));

  auto path = app.document().document().querySelector("#curve");
  ASSERT_TRUE(path.has_value());
  app.setSelection(*path);

  const SelectionChromeSnapshot snapshot = OverlayRenderer::captureChromeSnapshot(
      std::span<const svg::SVGElement>(app.selectedElements()), std::nullopt, Transform2d(),
      std::nullopt, std::span<const svg::SVGElement>(), std::nullopt, SelectionChromeDetail::Full);

  EXPECT_FALSE(snapshot.paths.empty());
  EXPECT_TRUE(snapshot.pathAnchorBoxesDoc.empty());
  EXPECT_TRUE(snapshot.pathControlLinesDoc.empty());
  EXPECT_TRUE(snapshot.pathControlPointBoxesDoc.empty());
}

TEST(OverlayRendererTest, NonPathSelectionsDoNotEmitPathPointChrome) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  auto rect = app.document().document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());
  app.setSelection(*rect);

  const SelectionChromeSnapshot snapshot = OverlayRenderer::captureChromeSnapshot(
      std::span<const svg::SVGElement>(app.selectedElements()), std::nullopt, Transform2d());

  EXPECT_FALSE(snapshot.paths.empty());
  EXPECT_TRUE(snapshot.pathAnchorBoxesDoc.empty());
  EXPECT_TRUE(snapshot.pathControlLinesDoc.empty());
  EXPECT_TRUE(snapshot.pathControlPointBoxesDoc.empty());
}

TEST(OverlayRendererTest, SelectionStrokeWidthScalesWithDevicePixelRatio) {
  const SelectionChromeSnapshot oneX = OverlayRenderer::captureChromeSnapshot(
      std::span<const svg::SVGElement>(), std::nullopt, Transform2d(), std::nullopt,
      std::span<const svg::SVGElement>(), std::nullopt, SelectionChromeDetail::Full, Transform2d(),
      std::nullopt, 1.0);
  const SelectionChromeSnapshot twoX = OverlayRenderer::captureChromeSnapshot(
      std::span<const svg::SVGElement>(), std::nullopt, Transform2d(), std::nullopt,
      std::span<const svg::SVGElement>(), std::nullopt, SelectionChromeDetail::Full, Transform2d(),
      std::nullopt, 2.0);

  EXPECT_GT(oneX.selectionStrokeWidthWorld, 1.0);
  EXPECT_DOUBLE_EQ(twoX.selectionStrokeWidthWorld, oneX.selectionStrokeWidthWorld * 2.0);
}

TEST(OverlayRendererTest, CaptureSnapshotCullsOffscreenSelectionAndHoverChrome) {
  constexpr std::string_view kSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 400 100">
              <rect id="visible" x="10" y="10" width="20" height="20" fill="red"/>
              <rect id="offscreen" x="250" y="10" width="20" height="20" fill="blue"/>
            </svg>)svg";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSvg));
  app.document().document().setCanvasSize(400, 100);

  auto visible = app.document().document().querySelector("#visible");
  auto offscreen = app.document().document().querySelector("#offscreen");
  ASSERT_TRUE(visible.has_value());
  ASSERT_TRUE(offscreen.has_value());
  app.setSelection(std::vector<svg::SVGElement>{*visible, *offscreen});
  const std::array<svg::SVGElement, 1> hoverElements{*offscreen};

  const SelectionChromeSnapshot snapshot = OverlayRenderer::captureChromeSnapshot(
      std::span<const svg::SVGElement>(app.selectedElements()), std::nullopt, Transform2d(),
      std::nullopt, std::span<const svg::SVGElement>(hoverElements),
      Box2d::FromXYWH(0.0, 0.0, 100.0, 100.0));

  ASSERT_EQ(snapshot.paths.size(), 1u);
  ASSERT_EQ(snapshot.aabbsDoc.size(), 1u);
  EXPECT_EQ(snapshot.aabbsDoc.front(), Box2d::FromXYWH(10.0, 10.0, 20.0, 20.0));
  EXPECT_TRUE(snapshot.hoverPaths.empty());
  EXPECT_TRUE(snapshot.hoverAabbsDoc.empty());
  EXPECT_EQ(snapshot.handleBoxesDoc.size(), 4u);
}

TEST(OverlayRendererTest, CaptureSnapshotCullsOffscreenHandles) {
  constexpr std::string_view kSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
              <rect id="target" x="0" y="0" width="100" height="100" fill="red"/>
            </svg>)svg";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSvg));
  app.document().document().setCanvasSize(100, 100);

  auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  app.setSelection(*target);

  const SelectionChromeSnapshot snapshot = OverlayRenderer::captureChromeSnapshot(
      std::span<const svg::SVGElement>(app.selectedElements()), std::nullopt, Transform2d(),
      std::nullopt, std::span<const svg::SVGElement>(), Box2d::FromXYWH(40.0, 40.0, 20.0, 20.0));

  EXPECT_EQ(snapshot.paths.size(), 1u);
  EXPECT_EQ(snapshot.aabbsDoc.size(), 1u);
  EXPECT_TRUE(snapshot.handleBoxesDoc.empty());
}

TEST(OverlayRendererTest, CaptureSnapshotCombinedBoundsOnlySkipsSelectionPaths) {
  constexpr std::string_view kSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
              <rect id="a" x="10" y="10" width="20" height="20" fill="red"/>
              <rect id="b" x="50" y="40" width="10" height="30" fill="blue"/>
            </svg>)svg";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSvg));
  app.document().document().setCanvasSize(100, 100);

  auto a = app.document().document().querySelector("#a");
  auto b = app.document().document().querySelector("#b");
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  app.setSelection(std::vector<svg::SVGElement>{*a, *b});

  const SelectionChromeSnapshot snapshot = OverlayRenderer::captureChromeSnapshot(
      std::span<const svg::SVGElement>(app.selectedElements()), std::nullopt, Transform2d(),
      std::nullopt, std::span<const svg::SVGElement>(), std::nullopt,
      SelectionChromeDetail::CombinedBoundsOnly);

  EXPECT_TRUE(snapshot.paths.empty());
  ASSERT_EQ(snapshot.aabbsDoc.size(), 1u);
  EXPECT_EQ(snapshot.aabbsDoc.front(), Box2d::FromXYWH(10.0, 10.0, 50.0, 60.0));
  EXPECT_EQ(snapshot.handleBoxesDoc.size(), 4u);
}

// The editor draws selection chrome into a *second* renderer's frame
// (separate from the document bitmap) so clicks don't pay a full SVG
// re-rasterize. This test mirrors that exact sequence: fresh Renderer,
// `beginFrame` directly, `drawChromeWithTransform`, `endFrame`,
// `takeSnapshot`. Used to crash on the first click because main.cc was
// touching `canvasFromDocumentTransform()` while the async render
// worker was busy; this test exercises the safe order, where the
// overlay render runs while the document is idle.
TEST(OverlayRendererTest, OverlayRendersIntoStandaloneRendererFrame) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));
  app.document().document().setCanvasSize(200, 200);

  auto rect = app.document().document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());
  app.setSelection(*rect);

  // Brand-new renderer - never had `draw(document)` called on it. This
  // is the configuration `main.cc` uses for the dedicated overlay
  // renderer, and was the source of the click-crash bug.
  svg::Renderer overlayRenderer;

  svg::RenderViewport viewport;
  viewport.size = Vector2d(200.0, 200.0);
  viewport.devicePixelRatio = 1.0;
  overlayRenderer.beginFrame(viewport);

  const Transform2d canvasFromDoc = app.document().document().canvasFromDocumentTransform();
  OverlayRenderer::drawChromeWithTransform(overlayRenderer, app.selectedElement(), canvasFromDoc);
  overlayRenderer.endFrame();

  const auto bitmap = overlayRenderer.takeSnapshot();
  ASSERT_FALSE(bitmap.empty());
  EXPECT_EQ(bitmap.dimensions.x, 200);
  EXPECT_EQ(bitmap.dimensions.y, 200);

  // Background should be transparent (alpha=0) almost everywhere; the
  // chrome stroke contributes a few non-zero alpha pixels around the
  // r1 rect's outline. Walk the buffer and confirm at least one pixel
  // has non-zero alpha so we know the overlay was actually drawn.
  bool foundOpaquePixel = false;
  for (std::size_t y = 0; y < static_cast<std::size_t>(bitmap.dimensions.y); ++y) {
    const std::uint8_t* row = bitmap.pixels.data() + y * bitmap.rowBytes;
    for (std::size_t x = 0; x < static_cast<std::size_t>(bitmap.dimensions.x); ++x) {
      if (row[x * 4 + 3] != 0) {
        foundOpaquePixel = true;
        break;
      }
    }
    if (foundOpaquePixel) {
      break;
    }
  }
  EXPECT_TRUE(foundOpaquePixel) << "Overlay bitmap is completely transparent - chrome was "
                                   "never drawn";
}

TEST(OverlayRendererTest, DrawsWhiteCornerHandlesWithSelectionStroke) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));
  app.document().document().setCanvasSize(200, 200);

  auto rect = app.document().document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());
  app.setSelection(*rect);

  svg::Renderer overlayRenderer;
  svg::RenderViewport viewport;
  viewport.size = Vector2d(200.0, 200.0);
  viewport.devicePixelRatio = 1.0;
  overlayRenderer.beginFrame(viewport);

  const Transform2d canvasFromDoc = app.document().document().canvasFromDocumentTransform();
  OverlayRenderer::drawChromeWithTransform(overlayRenderer, app.selectedElement(), canvasFromDoc);
  overlayRenderer.endFrame();

  const auto bitmap = overlayRenderer.takeSnapshot();
  ASSERT_FALSE(bitmap.empty());

  const auto pixelAt = [&](int x, int y, int channel) -> std::uint8_t {
    const std::uint8_t* row = bitmap.pixels.data() + y * bitmap.rowBytes;
    return row[x * 4 + channel];
  };

  // Top-left selection corner is also the center of the top-left handle.
  EXPECT_GT(pixelAt(20, 30, 0), 220);
  EXPECT_GT(pixelAt(20, 30, 1), 220);
  EXPECT_GT(pixelAt(20, 30, 2), 220);
  EXPECT_GT(pixelAt(20, 30, 3), 220);

  // At least one pixel on the handle border should carry the cyan selection
  // stroke color. Exact coverage varies slightly with rasterizer AA.
  bool foundCyanBorderPixel = false;
  for (int y = 24; y <= 36; ++y) {
    for (int x = 14; x <= 26; ++x) {
      const bool onBorder = x <= 16 || x >= 24 || y <= 26 || y >= 34;
      if (!onBorder) {
        continue;
      }

      if (pixelAt(x, y, 0) < 120 && pixelAt(x, y, 1) > 130 && pixelAt(x, y, 2) > 170 &&
          pixelAt(x, y, 3) > 120) {
        foundCyanBorderPixel = true;
      }
    }
  }
  EXPECT_TRUE(foundCyanBorderPixel);
}

TEST(OverlayRendererTest, ActiveRotationUsesOrientedBoundsUntilGestureEnds) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));
  app.document().document().setCanvasSize(200, 200);

  auto rect = app.document().document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());
  app.setSelection(*rect);

  const Transform2d canvasFromDoc = app.document().document().canvasFromDocumentTransform();
  const Box2d startBounds = Box2d::FromXYWH(20.0, 30.0, 40.0, 50.0);
  const Vector2d center(40.0, 55.0);
  const Transform2d rotatedDocumentFromStartDocument = Transform2d::Translate(-center) *
                                                       Transform2d::Rotate(kPi / 2.0) *
                                                       Transform2d::Translate(center);

  const SelectionChromeSnapshot activeSnapshot = OverlayRenderer::captureChromeSnapshot(
      std::span<const svg::SVGElement>(app.selectedElements()), std::nullopt, canvasFromDoc,
      SelectionChromeBoundsPreview{
          .startBoundsDoc = startBounds,
          .documentFromStartDocument = rotatedDocumentFromStartDocument,
      });

  ASSERT_TRUE(activeSnapshot.orientedBoundsDoc.has_value());
  ASSERT_EQ(activeSnapshot.handleBoxesDoc.size(), 4u);
  const Vector2d expectedTopLeft =
      rotatedDocumentFromStartDocument.transformPosition(startBounds.topLeft);
  EXPECT_THAT(activeSnapshot.orientedBoundsDoc->cornersDoc,
              ElementsAre(Vector2dNear(expectedTopLeft, 1e-6), _, _, _));

  const SelectionChromeSnapshot settledSnapshot = OverlayRenderer::captureChromeSnapshot(
      std::span<const svg::SVGElement>(app.selectedElements()), std::nullopt, canvasFromDoc);
  EXPECT_FALSE(settledSnapshot.orientedBoundsDoc.has_value());
  ASSERT_EQ(settledSnapshot.aabbsDoc.size(), 1u);
  EXPECT_EQ(settledSnapshot.aabbsDoc.front(), startBounds);
}

TEST(OverlayRendererTest, CaptureSnapshotCanProjectLiveDragBackToRepresentedDocument) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));
  app.document().document().setCanvasSize(200, 200);

  auto rect = app.document().document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());
  rect->cast<svg::SVGGraphicsElement>().setTransform(Transform2d::Translate(50.0, 0.0));
  (void)app.flushFrame();
  app.setSelection(*rect);

  const Transform2d canvasFromDoc = app.document().document().canvasFromDocumentTransform();
  const SelectionChromeSnapshot snapshot = OverlayRenderer::captureChromeSnapshot(
      std::span<const svg::SVGElement>(app.selectedElements()), std::nullopt, canvasFromDoc,
      std::nullopt, std::span<const svg::SVGElement>(),
      /*cullRectDoc=*/std::nullopt, SelectionChromeDetail::Full,
      Transform2d::Translate(-50.0, 0.0));

  ASSERT_EQ(snapshot.aabbsDoc.size(), 1u);
  EXPECT_EQ(snapshot.aabbsDoc.front(), Box2d::FromXYWH(20.0, 30.0, 40.0, 50.0));
  ASSERT_EQ(snapshot.paths.size(), 1u);
  EXPECT_EQ(snapshot.paths.front().pathDoc.bounds(), Box2d::FromXYWH(20.0, 30.0, 40.0, 50.0));
}

TEST(OverlayRendererTest, DrawChromeFromSnapshotCoversAuxiliaryChromeLayers) {
  SelectionChromeSnapshot snapshot;
  snapshot.canvasFromDoc = Transform2d();
  snapshot.selectionStrokeWidthWorld = 1.25;
  snapshot.hoverStrokeWidthWorld = 1.5;
  snapshot.marqueeStrokeWidthWorld = 1.5;
  snapshot.livePathPreview = SelectionChromeSnapshot::LivePathPreview{
      .pathDoc = RectPath(4.0, 4.0, 12.0, 8.0),
      .fillColor = css::RGBA(0x20, 0x80, 0xff, 0xff),
      .strokeColor = std::nullopt,
      .strokeWidthDoc = 1.0,
  };
  snapshot.hoverPaths.push_back(SelectionChromeSnapshot::PathItem{
      .pathDoc = RectPath(24.0, 4.0, 16.0, 12.0),
  });
  snapshot.pathControlLinesDoc.push_back(SelectionChromeSnapshot::PathControlLine{
      .anchorDoc = Vector2d(8.0, 36.0), .controlDoc = Vector2d(28.0, 24.0)});
  snapshot.pathControlPointBoxesDoc.push_back(Box2d::FromXYWH(24.0, 22.0, 4.0, 4.0));
  snapshot.pathAnchorBoxesDoc.push_back(Box2d::FromXYWH(6.0, 34.0, 5.0, 5.0));
  snapshot.penPreviewSegmentDoc = LinePath(Vector2d(44.0, 8.0), Vector2d(76.0, 24.0));
  snapshot.penCloseAffordanceDoc = Vector2d(78.0, 24.0);
  snapshot.orientedBoundsDoc = SelectionChromeSnapshot::OrientedBox{
      .cornersDoc =
          {
              Vector2d(12.0, 60.0),
              Vector2d(40.0, 54.0),
              Vector2d(46.0, 82.0),
              Vector2d(18.0, 88.0),
          },
  };
  snapshot.handleBoxesDoc.push_back(Box2d::FromXYWH(8.0, 56.0, 8.0, 8.0));
  snapshot.marqueeDoc = Box2d::FromXYWH(60.0, 60.0, 24.0, 18.0);

  const auto bitmap = DrawSnapshot(snapshot);

  ASSERT_FALSE(bitmap.empty());
  EXPECT_TRUE(HasAnyNonTransparentPixel(bitmap));
}

TEST(OverlayRendererTest, DrawChromeFromSnapshotCoversHoverBoundsAndStrokeOnlyPreview) {
  SelectionChromeSnapshot snapshot;
  snapshot.canvasFromDoc = Transform2d();
  snapshot.selectionStrokeWidthWorld = 1.25;
  snapshot.hoverStrokeWidthWorld = 1.5;
  snapshot.livePathPreview = SelectionChromeSnapshot::LivePathPreview{
      .pathDoc = RectPath(12.0, 12.0, 20.0, 16.0),
      .fillColor = std::nullopt,
      .strokeColor = css::RGBA(0xff, 0x40, 0x20, 0xff),
      .strokeWidthDoc = 2.0,
  };
  snapshot.hoverAabbsDoc.push_back(Box2d::FromXYWH(50.0, 12.0, 18.0, 18.0));

  const auto bitmap = DrawSnapshot(snapshot);

  ASSERT_FALSE(bitmap.empty());
  EXPECT_TRUE(HasAnyNonTransparentPixel(bitmap));
}

TEST(OverlayRendererTest, CaptureSnapshotCullsOffscreenActiveRotationBounds) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));
  app.document().document().setCanvasSize(200, 200);

  auto rect = app.document().document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());
  app.setSelection(*rect);

  const SelectionChromeSnapshot snapshot = OverlayRenderer::captureChromeSnapshot(
      std::span<const svg::SVGElement>(app.selectedElements()), std::nullopt, Transform2d(),
      SelectionChromeBoundsPreview{
          .startBoundsDoc = Box2d::FromXYWH(500.0, 500.0, 20.0, 20.0),
          .documentFromStartDocument = Transform2d(),
      },
      std::span<const svg::SVGElement>(), Box2d::FromXYWH(0.0, 0.0, 100.0, 100.0));

  EXPECT_FALSE(snapshot.orientedBoundsDoc.has_value());
  EXPECT_TRUE(snapshot.aabbsDoc.empty());
  EXPECT_TRUE(snapshot.handleBoxesDoc.empty());
}

TEST(OverlayRendererTest, CaptureLivePathPreviewAcceptsSolidPaintedPath) {
  constexpr std::string_view kPathSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
              <path id="path" d="M 10 10 L 40 10 L 40 30 Z" fill="#102030"
                    fill-opacity="0.5" stroke="#405060" stroke-width="3"
                    stroke-opacity="0.75" opacity="0.8"/>
            </svg>)svg";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kPathSvg));
  auto path = app.document().document().querySelector("#path");
  ASSERT_TRUE(path.has_value());

  const SelectionChromeSnapshot snapshot = CaptureLivePathPreview(*path);

  ASSERT_TRUE(snapshot.livePathPreview.has_value());
  EXPECT_FALSE(snapshot.livePathPreview->pathDoc.empty());
  EXPECT_EQ(snapshot.livePathPreview->fillColor, std::optional(css::RGBA(0x10, 0x20, 0x30, 0xff)));
  EXPECT_EQ(snapshot.livePathPreview->strokeColor,
            std::optional(css::RGBA(0x40, 0x50, 0x60, 0xff)));
  EXPECT_DOUBLE_EQ(snapshot.livePathPreview->strokeWidthDoc, 3.0);
  EXPECT_DOUBLE_EQ(snapshot.livePathPreview->opacity, 0.8);
  EXPECT_DOUBLE_EQ(snapshot.livePathPreview->fillOpacity, 0.5);
  EXPECT_DOUBLE_EQ(snapshot.livePathPreview->strokeOpacity, 0.75);
}

TEST(OverlayRendererTest, CaptureLivePathPreviewRejectsUnsupportedInputs) {
  constexpr std::string_view kSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
              <defs>
                <linearGradient id="grad"><stop offset="0" stop-color="red"/></linearGradient>
                <clipPath id="clip"><rect width="10" height="10"/></clipPath>
              </defs>
              <g id="group"><path d="M 1 1 L 2 2"/></g>
              <path id="empty" d=""/>
              <path id="gradient" d="M 10 10 L 20 10 L 20 20 Z" fill="url(#grad)"/>
              <path id="clipped" d="M 30 10 L 40 10 L 40 20 Z" clip-path="url(#clip)"/>
              <path id="percent" d="M 50 10 L 60 10 L 60 20 Z" fill="none"
                    stroke="black" stroke-width="10%"/>
            </svg>)svg";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSvg));

  for (std::string_view selector : {"#group", "#empty", "#gradient", "#clipped", "#percent"}) {
    auto element = app.document().document().querySelector(selector);
    ASSERT_TRUE(element.has_value()) << selector;
    EXPECT_FALSE(CaptureLivePathPreview(*element).livePathPreview.has_value()) << selector;
  }
}

// Calling the overlay-only render path repeatedly (the click→reselect
// loop) must not crash and must keep producing well-formed bitmaps.
// The previous bug surfaced on the *second* click; this regression
// test exercises the same call pattern several times in a row with
// changing selection state.
TEST(OverlayRendererTest, RepeatedOverlayRendersWithChangingSelection) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));
  app.document().document().setCanvasSize(200, 200);

  auto r1 = app.document().document().querySelector("#r1");
  ASSERT_TRUE(r1.has_value());

  svg::Renderer overlayRenderer;
  svg::RenderViewport viewport;
  viewport.size = Vector2d(200.0, 200.0);
  viewport.devicePixelRatio = 1.0;

  const auto runOverlayPass = [&](const std::optional<svg::SVGElement>& selection) {
    overlayRenderer.beginFrame(viewport);
    const Transform2d canvasFromDoc = app.document().document().canvasFromDocumentTransform();
    OverlayRenderer::drawChromeWithTransform(overlayRenderer, selection, canvasFromDoc);
    overlayRenderer.endFrame();
    auto bitmap = overlayRenderer.takeSnapshot();
    EXPECT_FALSE(bitmap.empty());
    EXPECT_EQ(bitmap.dimensions.x, 200);
    EXPECT_EQ(bitmap.dimensions.y, 200);
  };

  // Frame 1: nothing selected - overlay must still produce a (blank,
  // transparent) bitmap without crashing.
  runOverlayPass(std::nullopt);

  // Frame 2: select r1 - overlay produces chrome.
  app.setSelection(*r1);
  runOverlayPass(app.selectedElement());

  // Frame 3: clear the selection - overlay back to blank.
  app.setSelection(std::nullopt);
  runOverlayPass(app.selectedElement());

  // Frame 4: re-select r1 - should match frame 2's behavior.
  app.setSelection(*r1);
  runOverlayPass(app.selectedElement());
}

// Regression for "selecting and moving a shape doesn't move the path
// outline, only the bounding box". The old OverlayRenderer drew the
// spline through `setTransform(canvasFromDoc)` directly, but
// `computedSpline()` returns *local* coordinates (before the element's
// own transform), so for any element with a non-identity transform the
// path stayed put while the AABB tracked the move correctly.
//
// This test loads a rect with a non-identity translate, snapshots the
// overlay bitmap, then verifies that the bitmap actually contains
// pixels in the post-translate region (i.e. the path was composed
// through worldFromLocal before going to canvas).
TEST(OverlayRendererTest, PathOutlineFollowsElementTransform) {
  // Document defines an 80×40 rect at (10, 20) with an extra
  // translate(60, 40), so its world-space top-left is (70, 60) and
  // bottom-right is (150, 100). The canvas is exactly the viewBox
  // size so canvasFromDoc is identity - that lets us assert directly
  // on bitmap pixel coordinates without worrying about scale.
  constexpr std::string_view kTransformedSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
              <rect id="movable" x="10" y="20" width="80" height="40"
                    fill="green" transform="translate(60 40)"/>
            </svg>)svg";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTransformedSvg));
  app.document().document().setCanvasSize(200, 200);

  auto rect = app.document().document().querySelector("#movable");
  ASSERT_TRUE(rect.has_value());
  app.setSelection(*rect);

  svg::Renderer overlayRenderer;
  svg::RenderViewport viewport;
  viewport.size = Vector2d(200.0, 200.0);
  viewport.devicePixelRatio = 1.0;
  overlayRenderer.beginFrame(viewport);

  const Transform2d canvasFromDoc = app.document().document().canvasFromDocumentTransform();
  OverlayRenderer::drawChromeWithTransform(overlayRenderer, app.selectedElement(), canvasFromDoc);
  overlayRenderer.endFrame();

  const auto bitmap = overlayRenderer.takeSnapshot();
  ASSERT_FALSE(bitmap.empty());

  // Helper: returns the alpha at a given canvas pixel.
  const auto alphaAt = [&](int x, int y) -> std::uint8_t {
    const std::uint8_t* row = bitmap.pixels.data() + y * bitmap.rowBytes;
    return row[x * 4 + 3];
  };

  // The chrome should hit pixels along the *world-space* rect outline:
  //   x ∈ {70, 150}, y ∈ [60, 100]      (vertical edges)
  //   y ∈ {60, 100}, x ∈ [70, 150]      (horizontal edges)
  // Probe a handful of points along each edge - at least one must
  // have non-zero alpha. We also assert that points far from the
  // *local* (untranslated) location at (10, 20)→(90, 60) are clean,
  // which catches the regression where the path stayed at local
  // coordinates instead of moving with the transform.
  bool foundOnTransformedEdge = false;
  // Top edge of transformed rect: y=60, x ∈ [70, 150]. Sample the
  // midpoint and a couple of spots.
  for (int x : {70, 110, 150}) {
    if (alphaAt(x, 60) > 0) {
      foundOnTransformedEdge = true;
      break;
    }
  }
  EXPECT_TRUE(foundOnTransformedEdge)
      << "Path outline / AABB did not appear at the *transformed* element location - the "
         "renderer is still treating the spline as world coordinates";

  // Sanity check that the *local* (un-translated) area is mostly
  // empty. A well-behaved overlay touches the pre-translate rect at
  // (10..90, 20..60) only if the bounding box happens to overlap
  // (which it doesn't for translate(60, 40)). Pick a spot that's
  // clearly outside the transformed AABB to confirm.
  EXPECT_EQ(alphaAt(20, 30), 0)
      << "Pixel at the *un-transformed* rect interior is non-empty - looks like the "
         "old local-space path is still being drawn";
}

TEST(OverlayRendererTest, DisplayNoneSelectionStillDrawsPathOutline) {
  constexpr std::string_view kHiddenSelectionSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
              <rect id="hidden" x="20" y="20" width="40" height="30"
                    fill="red" style="display:none"/>
            </svg>)svg";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kHiddenSelectionSvg));
  app.document().document().setCanvasSize(100, 100);

  auto hidden = app.document().document().querySelector("#hidden");
  ASSERT_TRUE(hidden.has_value());
  app.setSelection(*hidden);

  svg::Renderer overlayRenderer;
  svg::RenderViewport viewport;
  viewport.size = Vector2d(100.0, 100.0);
  viewport.devicePixelRatio = 1.0;
  overlayRenderer.beginFrame(viewport);

  const Transform2d canvasFromDoc = app.document().document().canvasFromDocumentTransform();
  OverlayRenderer::drawChromeWithTransform(overlayRenderer, app.selectedElement(), canvasFromDoc);
  overlayRenderer.endFrame();

  const auto bitmap = overlayRenderer.takeSnapshot();
  ASSERT_FALSE(bitmap.empty());

  const auto pixelAt = [&](int x, int y) -> std::array<std::uint8_t, 4> {
    const std::uint8_t* row = bitmap.pixels.data() + y * bitmap.rowBytes;
    return {row[x * 4 + 0], row[x * 4 + 1], row[x * 4 + 2], row[x * 4 + 3]};
  };

  bool foundOutline = false;
  std::array<std::uint8_t, 4> outlinePixel{};
  for (int x = 20; x <= 60; ++x) {
    const std::array<std::uint8_t, 4> pixel = pixelAt(x, 20);
    if (pixel[3] > 0) {
      foundOutline = true;
      outlinePixel = pixel;
      break;
    }
  }
  EXPECT_TRUE(foundOutline)
      << "A display:none selection should still expose its path overlay outline for source edits.";
  if (foundOutline) {
    EXPECT_THAT(outlinePixel, DimmedGrayBlueStrokePixel())
        << "Display-none selection chrome should use the dimmed gray-blue stroke, not pure cyan.";
  }
}

TEST(OverlayRendererTest, DisplayNonePathSelectionStillDrawsPathOutline) {
  constexpr std::string_view kHiddenPathSelectionSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
              <style>.cls-82{fill:red}</style>
              <path class="cls-82" d="M20 20 H60 V60 H20 Z" style="display:none"/>
            </svg>)svg";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kHiddenPathSelectionSvg));
  app.document().document().setCanvasSize(100, 100);

  auto hidden = app.document().document().querySelector(".cls-82");
  ASSERT_TRUE(hidden.has_value());
  app.setSelection(*hidden);

  svg::Renderer overlayRenderer;
  svg::RenderViewport viewport;
  viewport.size = Vector2d(100.0, 100.0);
  viewport.devicePixelRatio = 1.0;
  overlayRenderer.beginFrame(viewport);

  const Transform2d canvasFromDoc = app.document().document().canvasFromDocumentTransform();
  OverlayRenderer::drawChromeWithTransform(overlayRenderer, app.selectedElement(), canvasFromDoc);
  overlayRenderer.endFrame();

  const auto bitmap = overlayRenderer.takeSnapshot();
  ASSERT_FALSE(bitmap.empty());

  const auto alphaAt = [&](int x, int y) -> std::uint8_t {
    const std::uint8_t* row = bitmap.pixels.data() + y * bitmap.rowBytes;
    return row[x * 4 + 3];
  };

  bool foundOutline = false;
  for (int x = 20; x <= 60; ++x) {
    foundOutline = foundOutline || alphaAt(x, 20) > 0;
  }
  EXPECT_TRUE(foundOutline)
      << "A display:none path selection should still expose its path overlay outline.";
}

// Regression for "dragging a shape moves the path outline in the
// *opposite* direction". Pre-fix, `DrawElementPathOutline` had an
// extraneous `.inverse()` on `geometry.elementFromWorld()`, so a
// rect translated by (+60, +40) had its path outline drawn at
// canvas (-60, -40). The `PathOutlineFollowsElementTransform` test
// above didn't catch the sign flip because it uses a plain rect
// whose AABB corners coincide with the outline corners - the AABB
// stroke lit up the expected pixels even when the outline was in
// the wrong place.
//
// This test uses a path that *doesn't* coincide with its AABB: an
// 'X' across the rect (top-left → bottom-right, plus top-right →
// bottom-left). The AABB is a rect (20..60 horizontal, 20..60
// vertical), but the outline lives only on the two diagonals. We
// assert that the outline's *midpoint* - which lies on neither
// AABB edge - gets stroke pixels at the transformed location.
TEST(OverlayRendererTest, PathOutlineDrawnAtTransformedLocationNotInverted) {
  // 40x40 "X" path at (20, 20) with translate(40, 30), so its
  // world-space bounds are (60, 50) to (100, 90) and the center
  // (where the two diagonals cross) is at (80, 70). The AABB
  // corners are (60, 50), (100, 50), (60, 90), (100, 90) - none of
  // them coincide with the diagonals' midpoint.
  constexpr std::string_view kDiagonalSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
              <path id="x" d="M20 20 L60 60 M60 20 L20 60"
                    fill="none" stroke="red" stroke-width="1"
                    transform="translate(40 30)"/>
            </svg>)svg";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kDiagonalSvg));
  app.document().document().setCanvasSize(200, 200);

  auto xPath = app.document().document().querySelector("#x");
  ASSERT_TRUE(xPath.has_value());
  app.setSelection(*xPath);

  svg::Renderer overlayRenderer;
  svg::RenderViewport viewport;
  viewport.size = Vector2d(200.0, 200.0);
  viewport.devicePixelRatio = 1.0;
  overlayRenderer.beginFrame(viewport);

  const Transform2d canvasFromDoc = app.document().document().canvasFromDocumentTransform();
  OverlayRenderer::drawChromeWithTransform(overlayRenderer, app.selectedElement(), canvasFromDoc);
  overlayRenderer.endFrame();

  const auto bitmap = overlayRenderer.takeSnapshot();
  ASSERT_FALSE(bitmap.empty());

  const auto alphaAt = [&](int x, int y) -> std::uint8_t {
    const std::uint8_t* row = bitmap.pixels.data() + y * bitmap.rowBytes;
    return row[x * 4 + 3];
  };

  // The diagonals cross at world (80, 70). Check pixels in a tiny
  // square around that point - at least one must be non-zero because
  // both diagonal strokes pass through it.
  bool foundOnTransformedDiagonal = false;
  for (int y = 68; y <= 72 && !foundOnTransformedDiagonal; ++y) {
    for (int x = 78; x <= 82 && !foundOnTransformedDiagonal; ++x) {
      if (alphaAt(x, y) > 0) {
        foundOnTransformedDiagonal = true;
      }
    }
  }
  EXPECT_TRUE(foundOnTransformedDiagonal)
      << "Path outline didn't appear at the transformed diagonal center (80, 70). "
         "Likely the overlay is still applying elementFromWorld().inverse() and "
         "drawing the outline at the reflected position.";

  // The INVERTED position of the same point (where the bug would
  // draw the stroke) is local (40, 40) → world (0, 10) because
  // inverted translate(-40, -30). Assert nothing's there.
  EXPECT_EQ(alphaAt(0, 10), 0)
      << "Pixel at the inverted-translate location is non-empty - overlay is drawing "
         "the path at the sign-flipped transform location.";
}

TEST(OverlayRendererTest, PathOutlineStrokeDoesNotInheritElementScale) {
  constexpr std::string_view kScaledInternalLineSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
              <path id="scaled" d="M 5 5 L 30 30 M 5 15 L 30 15"
                    fill="none" stroke="red" stroke-width="1"
                    transform="scale(6)"/>
            </svg>)svg";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kScaledInternalLineSvg));
  app.document().document().setCanvasSize(200, 200);

  auto scaledPath = app.document().document().querySelector("#scaled");
  ASSERT_TRUE(scaledPath.has_value());
  app.setSelection(*scaledPath);

  svg::Renderer overlayRenderer;
  svg::RenderViewport viewport;
  viewport.size = Vector2d(200.0, 200.0);
  viewport.devicePixelRatio = 1.0;
  overlayRenderer.beginFrame(viewport);

  const Transform2d canvasFromDoc = app.document().document().canvasFromDocumentTransform();
  OverlayRenderer::drawChromeWithTransform(overlayRenderer, app.selectedElement(), canvasFromDoc);
  overlayRenderer.endFrame();

  const auto bitmap = overlayRenderer.takeSnapshot();
  ASSERT_FALSE(bitmap.empty());

  const auto alphaAt = [&](int x, int y) -> std::uint8_t {
    const std::uint8_t* row = bitmap.pixels.data() + y * bitmap.rowBytes;
    return row[x * 4 + 3];
  };

  EXPECT_GT(alphaAt(60, 90), 0) << "scaled horizontal path outline should be visible";
  EXPECT_EQ(alphaAt(60, 92), 0)
      << "selection path chrome stroke should stay screen-space thin; it must not be scaled by "
         "the selected element's transform";
}

// ---------------------------------------------------------------------------
// Multi-element path-outline chrome.
// ---------------------------------------------------------------------------

TEST(OverlayRendererTest, MultiElementSpanDrawsPathOutlinesForEachSelectedElement) {
  constexpr std::string_view kTwoRectsSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
              <rect id="r1" x="10"  y="10"  width="40" height="40" fill="red"/>
              <rect id="r2" x="100" y="100" width="40" height="40" fill="blue"/>
            </svg>)svg";
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectsSvg));
  app.document().document().setCanvasSize(200, 200);

  auto r1 = app.document().document().querySelector("#r1");
  auto r2 = app.document().document().querySelector("#r2");
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());
  app.setSelection(std::vector<svg::SVGElement>{*r1, *r2});

  svg::Renderer overlayRenderer;
  svg::RenderViewport viewport;
  viewport.size = Vector2d(200.0, 200.0);
  viewport.devicePixelRatio = 1.0;
  overlayRenderer.beginFrame(viewport);

  const Transform2d canvasFromDoc = app.document().document().canvasFromDocumentTransform();
  OverlayRenderer::drawChromeWithTransform(
      overlayRenderer, std::span<const svg::SVGElement>(app.selectedElements()), canvasFromDoc);
  overlayRenderer.endFrame();

  const auto bitmap = overlayRenderer.takeSnapshot();
  ASSERT_FALSE(bitmap.empty());

  // Helper: returns the alpha at a given canvas pixel.
  const auto alphaAt = [&](int x, int y) -> std::uint8_t {
    const std::uint8_t* row = bitmap.pixels.data() + y * bitmap.rowBytes;
    return row[x * 4 + 3];
  };

  // Both rects must contribute pixels. Probe the corners of each
  // rect's outline - at least one corner pixel from each rect should
  // be non-zero alpha after the overlay drew both path outlines.
  const auto anyNonZero = [&](int xMin, int yMin, int xMax, int yMax) {
    for (int y = yMin; y <= yMax; ++y) {
      for (int x = xMin; x <= xMax; ++x) {
        if (alphaAt(x, y) > 0) {
          return true;
        }
      }
    }
    return false;
  };

  EXPECT_TRUE(anyNonZero(8, 8, 12, 12)) << "no chrome around r1 top-left corner";
  EXPECT_TRUE(anyNonZero(98, 98, 102, 102)) << "no chrome around r2 top-left corner";
}

// Multi-select chrome: per-element path outlines + per-element AABBs +
// a single combined AABB across the whole selection, all drawn in the
// Skia overlay. Historically the combined AABB lived on the ImGui draw
// list (`RenderPanePresenter`) and an earlier test asserted the overlay
// stayed path-only; that design was folded back into the overlay so
// Geode can own the whole chrome layer end-to-end. This test locks in
// the unified behavior: the combined AABB corners (which are NOT on any
// per-element outline) must appear in the overlay bitmap.
//
// Bounds are now computed inline by `OverlayRenderer::drawChrome*`
// (same call `SnapshotSelectionWorldBounds` makes) so the combined
// AABB matches the current DOM transform every frame - no more
// frame-lag shear against the path outlines.
TEST(OverlayRendererTest, MultiSelectDrawsCombinedAabbInSkiaOverlay) {
  constexpr std::string_view kTwoRectsSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
              <rect id="r1" x="20"  y="20"  width="20" height="20" fill="red"/>
              <rect id="r2" x="160" y="160" width="20" height="20" fill="blue"/>
            </svg>)svg";
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectsSvg));
  app.document().document().setCanvasSize(200, 200);

  auto r1 = app.document().document().querySelector("#r1");
  auto r2 = app.document().document().querySelector("#r2");
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());
  app.setSelection(std::vector<svg::SVGElement>{*r1, *r2});

  svg::Renderer overlayRenderer;
  svg::RenderViewport viewport;
  viewport.size = Vector2d(200.0, 200.0);
  viewport.devicePixelRatio = 1.0;
  overlayRenderer.beginFrame(viewport);

  const Transform2d canvasFromDoc = app.document().document().canvasFromDocumentTransform();
  OverlayRenderer::drawChromeWithTransform(
      overlayRenderer, std::span<const svg::SVGElement>(app.selectedElements()), canvasFromDoc);
  overlayRenderer.endFrame();

  const auto bitmap = overlayRenderer.takeSnapshot();
  ASSERT_FALSE(bitmap.empty());
  const auto alphaAt = [&](int x, int y) {
    const std::uint8_t* row = bitmap.pixels.data() + y * bitmap.rowBytes;
    return row[x * 4 + 3];
  };

  // The combined AABB of (20..40, 20..40) ∪ (160..180, 160..180) is
  // (20..180, 20..180). Its corners at (180, 20) and (20, 180) are
  // INSIDE the combined rect but OUTSIDE both per-element outlines -
  // so they only get pixels when the combined AABB is drawn. Under
  // the unified overlay, they MUST light up.
  const auto anyNonZeroNear = [&](int cx, int cy, int radius) {
    for (int y = cy - radius; y <= cy + radius; ++y) {
      for (int x = cx - radius; x <= cx + radius; ++x) {
        if (x >= 0 && y >= 0 && x < bitmap.dimensions.x && y < bitmap.dimensions.y &&
            alphaAt(x, y) > 0) {
          return true;
        }
      }
    }
    return false;
  };
  EXPECT_TRUE(anyNonZeroNear(180, 20, 2))
      << "combined-AABB top-right corner missing from the unified Skia overlay";
  EXPECT_TRUE(anyNonZeroNear(20, 180, 2))
      << "combined-AABB bottom-left corner missing from the unified Skia overlay";
}

// Marquee chrome also moved to the ImGui draw list. The path overlay
// should stay transparent when there is no selected geometry.
// Selecting a `<g filter="...">` elevates picker-wise to the group, but
// the group itself isn't a geometry element - pre-fix, the overlay
// drew nothing. Users expect selection chrome to show the outlines of
// every visible shape inside the group, matching established vector-editor
// behavior. The AABB must also envelope the full group, not come back empty.
TEST(OverlayRendererTest, SelectingFilterGroupDrawsOutlinesOfAllGeometryDescendants) {
  constexpr std::string_view kFilterGroupSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
              <defs>
                <filter id="blur"><feGaussianBlur stdDeviation="2"/></filter>
              </defs>
              <g id="grp" filter="url(#blur)">
                <rect id="child_a" x="20"  y="20"  width="40" height="40" fill="red"/>
                <rect id="child_b" x="140" y="140" width="30" height="30" fill="blue"/>
              </g>
            </svg>)svg";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kFilterGroupSvg));
  app.document().document().setCanvasSize(200, 200);

  auto group = app.document().document().querySelector("#grp");
  ASSERT_TRUE(group.has_value());
  app.setSelection(*group);

  svg::Renderer overlayRenderer;
  svg::RenderViewport viewport;
  viewport.size = Vector2d(200.0, 200.0);
  viewport.devicePixelRatio = 1.0;
  overlayRenderer.beginFrame(viewport);

  const Transform2d canvasFromDoc = app.document().document().canvasFromDocumentTransform();
  OverlayRenderer::drawChromeWithTransform(
      overlayRenderer, std::span<const svg::SVGElement>(app.selectedElements()), canvasFromDoc);
  overlayRenderer.endFrame();

  const auto bitmap = overlayRenderer.takeSnapshot();
  ASSERT_FALSE(bitmap.empty());
  const auto alphaAt = [&](int x, int y) -> std::uint8_t {
    const std::uint8_t* row = bitmap.pixels.data() + y * bitmap.rowBytes;
    return row[x * 4 + 3];
  };
  const auto anyNonZeroNear = [&](int cx, int cy, int radius) {
    for (int y = cy - radius; y <= cy + radius; ++y) {
      for (int x = cx - radius; x <= cx + radius; ++x) {
        if (x >= 0 && y >= 0 && x < bitmap.dimensions.x && y < bitmap.dimensions.y &&
            alphaAt(x, y) > 0) {
          return true;
        }
      }
    }
    return false;
  };

  // Outlines of both child rects must appear. Each corner is OUTSIDE
  // the *other* rect's AABB envelope, so a missing per-child outline
  // would leave it empty even with an all-encompassing group AABB.
  EXPECT_TRUE(anyNonZeroNear(20, 20, 2)) << "child_a top-left outline missing";
  EXPECT_TRUE(anyNonZeroNear(60, 60, 2)) << "child_a bottom-right outline missing";
  EXPECT_TRUE(anyNonZeroNear(140, 140, 2)) << "child_b top-left outline missing";
  EXPECT_TRUE(anyNonZeroNear(170, 170, 2)) << "child_b bottom-right outline missing";
}

// When a group has `<defs>` / `<clipPath>` / `<filter>` children, the
// shapes inside those containers are paint-server or clip definitions,
// not part of the visual tree. Selecting the group must NOT decorate
// them - only the sibling geometry that actually paints.
TEST(OverlayRendererTest, SelectingGroupSkipsNonRenderedContainerChildren) {
  constexpr std::string_view kSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
              <g id="grp">
                <defs>
                  <clipPath id="clip">
                    <rect id="hidden" x="5" y="5" width="10" height="10"/>
                  </clipPath>
                </defs>
                <rect id="visible" x="80" y="80" width="40" height="40" fill="green"/>
              </g>
            </svg>)svg";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSvg));
  app.document().document().setCanvasSize(200, 200);

  auto group = app.document().document().querySelector("#grp");
  ASSERT_TRUE(group.has_value());
  app.setSelection(*group);

  svg::Renderer overlayRenderer;
  svg::RenderViewport viewport;
  viewport.size = Vector2d(200.0, 200.0);
  viewport.devicePixelRatio = 1.0;
  overlayRenderer.beginFrame(viewport);

  const Transform2d canvasFromDoc = app.document().document().canvasFromDocumentTransform();
  OverlayRenderer::drawChromeWithTransform(
      overlayRenderer, std::span<const svg::SVGElement>(app.selectedElements()), canvasFromDoc);
  overlayRenderer.endFrame();

  const auto bitmap = overlayRenderer.takeSnapshot();
  ASSERT_FALSE(bitmap.empty());
  const auto alphaAt = [&](int x, int y) -> std::uint8_t {
    const std::uint8_t* row = bitmap.pixels.data() + y * bitmap.rowBytes;
    return row[x * 4 + 3];
  };

  // The clipPath's internal rect at (5,5)-(15,15) must NOT produce
  // chrome. The visible rect at (80,80)-(120,120) must. Probe well
  // inside the hidden rect - if the recursion leaked into <defs> we'd
  // find cyan stroke here.
  for (int y = 6; y <= 14; ++y) {
    for (int x = 6; x <= 14; ++x) {
      ASSERT_EQ(alphaAt(x, y), 0) << "chrome appeared inside a <defs><clipPath> subtree at (" << x
                                  << ", " << y << ")";
    }
  }
  bool foundVisibleOutline = false;
  for (int x = 78; x <= 82 && !foundVisibleOutline; ++x) {
    for (int y = 78; y <= 82 && !foundVisibleOutline; ++y) {
      if (alphaAt(x, y) > 0) {
        foundVisibleOutline = true;
      }
    }
  }
  EXPECT_TRUE(foundVisibleOutline) << "visible rect's outline is missing from the overlay";
}

TEST(OverlayRendererTest, EmptySelectionSpanProducesTransparentOverlay) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));
  app.document().document().setCanvasSize(200, 200);

  svg::Renderer overlayRenderer;
  svg::RenderViewport viewport;
  viewport.size = Vector2d(200.0, 200.0);
  viewport.devicePixelRatio = 1.0;
  overlayRenderer.beginFrame(viewport);

  const Transform2d canvasFromDoc = app.document().document().canvasFromDocumentTransform();
  OverlayRenderer::drawChromeWithTransform(overlayRenderer, std::span<const svg::SVGElement>(),
                                           canvasFromDoc);
  overlayRenderer.endFrame();

  const auto bitmap = overlayRenderer.takeSnapshot();
  ASSERT_FALSE(bitmap.empty());

  for (std::size_t y = 0; y < static_cast<std::size_t>(bitmap.dimensions.y); ++y) {
    const std::uint8_t* row = bitmap.pixels.data() + y * bitmap.rowBytes;
    for (std::size_t x = 0; x < static_cast<std::size_t>(bitmap.dimensions.x); ++x) {
      ASSERT_EQ(row[x * 4 + 3], 0) << "at (" << x << ", " << y << ")";
    }
  }
}

TEST(OverlayRendererTest, EmptySelectionSpanWithIdentityTransformIsNoOp) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));
  app.document().document().setCanvasSize(200, 200);

  svg::Renderer overlayRenderer;
  svg::RenderViewport viewport;
  viewport.size = Vector2d(200.0, 200.0);
  viewport.devicePixelRatio = 1.0;
  overlayRenderer.beginFrame(viewport);

  // Both inputs empty - should do nothing without crashing.
  OverlayRenderer::drawChromeWithTransform(overlayRenderer, std::span<const svg::SVGElement>(),
                                           Transform2d());
  overlayRenderer.endFrame();

  const auto bitmap = overlayRenderer.takeSnapshot();
  ASSERT_FALSE(bitmap.empty());
  // Bitmap should be entirely transparent (the beginFrame clear).
  for (std::size_t y = 0; y < static_cast<std::size_t>(bitmap.dimensions.y); ++y) {
    const std::uint8_t* row = bitmap.pixels.data() + y * bitmap.rowBytes;
    for (std::size_t x = 0; x < static_cast<std::size_t>(bitmap.dimensions.x); ++x) {
      ASSERT_EQ(row[x * 4 + 3], 0) << "at (" << x << ", " << y << ")";
    }
  }
}

TEST(OverlayRendererTest, ToleratesStaleSelectionAfterReload) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  auto rect = app.document().document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());
  app.setSelection(*rect);

  // Reload the document - the previously-selected entity is now stale.
  // EditorApp::loadFromString clears the selection, so this case is
  // handled by the public API; test it explicitly to lock in the
  // contract.
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));
  EXPECT_FALSE(app.hasSelection());

  svg::Renderer renderer;
  renderer.draw(app.document().document());
  OverlayRenderer::drawChrome(renderer, app);
  SUCCEED();
}

// Design doc 0033 §M7: `captureChromeSnapshot` + `drawChromeFromSnapshot`
// must produce byte-identical pixels to the live `drawChromeWithTransform`
// path. Pins the invariant - any future divergence (e.g. a missed paint
// or a stroke-width regression) trips this test before it ships.
TEST(OverlayRendererTest, SnapshotProducesByteIdenticalPixelsAsLivePath) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));
  app.document().document().setCanvasSize(200, 200);
  auto rect = app.document().document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());
  app.setSelection(*rect);

  svg::RenderViewport viewport;
  viewport.size = Vector2d(200.0, 200.0);
  viewport.devicePixelRatio = 1.0;
  const Transform2d canvasFromDoc = app.document().document().canvasFromDocumentTransform();
  const std::vector<svg::SVGElement> selection = app.selectedElements();

  // Live path - routes through capture+draw internally now.
  svg::Renderer liveRenderer;
  liveRenderer.beginFrame(viewport);
  OverlayRenderer::drawChromeWithTransform(liveRenderer,
                                           std::span<const svg::SVGElement>(selection),
                                           /*marqueeRectDoc=*/std::nullopt, canvasFromDoc);
  liveRenderer.endFrame();
  const auto liveBitmap = liveRenderer.takeSnapshot();

  // Snapshot path - explicitly invoked.
  const SelectionChromeSnapshot snapshot = OverlayRenderer::captureChromeSnapshot(
      std::span<const svg::SVGElement>(selection), /*marqueeRectDoc=*/std::nullopt, canvasFromDoc);
  svg::Renderer snapshotRenderer;
  snapshotRenderer.beginFrame(viewport);
  OverlayRenderer::drawChromeFromSnapshot(snapshotRenderer, snapshot);
  snapshotRenderer.endFrame();
  const auto snapshotBitmap = snapshotRenderer.takeSnapshot();

  ASSERT_EQ(liveBitmap.dimensions, snapshotBitmap.dimensions);
  ASSERT_EQ(liveBitmap.pixels.size(), snapshotBitmap.pixels.size());
  EXPECT_EQ(liveBitmap.pixels, snapshotBitmap.pixels)
      << "Live and snapshot chrome paths must produce identical pixels.";
}

// Critical race-safety property: a snapshot captured at frame N must
// remain visually correct even if the registry mutates between capture
// and draw. Without this guarantee, the worker mutating the document
// mid-chrome-rasterize would produce garbage or a crash. Today this
// holds because the snapshot holds path data by value - no registry
// pointers survive past `captureChromeSnapshot`.
TEST(OverlayRendererTest, SnapshotSurvivesDocumentMutationBetweenCaptureAndDraw) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));
  app.document().document().setCanvasSize(200, 200);
  auto rect = app.document().document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());
  app.setSelection(*rect);

  svg::RenderViewport viewport;
  viewport.size = Vector2d(200.0, 200.0);
  viewport.devicePixelRatio = 1.0;
  const Transform2d canvasFromDoc = app.document().document().canvasFromDocumentTransform();
  const std::vector<svg::SVGElement> selection = app.selectedElements();

  // Capture at the original position.
  const SelectionChromeSnapshot snapshot = OverlayRenderer::captureChromeSnapshot(
      std::span<const svg::SVGElement>(selection), /*marqueeRectDoc=*/std::nullopt, canvasFromDoc);

  // Take a reference rasterize before mutating anything, so we have a
  // pixel-exact "what does this snapshot's output look like" baseline.
  svg::Renderer baselineRenderer;
  baselineRenderer.beginFrame(viewport);
  OverlayRenderer::drawChromeFromSnapshot(baselineRenderer, snapshot);
  baselineRenderer.endFrame();
  const auto baselineBitmap = baselineRenderer.takeSnapshot();

  // Mutate the document AFTER capture. If the snapshot held registry
  // pointers, the next draw could pick up the mutated transform; the
  // test pins that it does NOT.
  rect->cast<svg::SVGGraphicsElement>().setTransform(Transform2d::Translate(50.0, 75.0));

  svg::Renderer postMutationRenderer;
  postMutationRenderer.beginFrame(viewport);
  OverlayRenderer::drawChromeFromSnapshot(postMutationRenderer, snapshot);
  postMutationRenderer.endFrame();
  const auto postMutationBitmap = postMutationRenderer.takeSnapshot();

  ASSERT_EQ(baselineBitmap.pixels.size(), postMutationBitmap.pixels.size());
  EXPECT_EQ(baselineBitmap.pixels, postMutationBitmap.pixels)
      << "Snapshot draw must be unaffected by post-capture registry mutations.";
}

// Without a locked-rejection flash the snapshot carries no flash payload - the
// red outline must only appear when the user clicks a locked element.
TEST(OverlayRendererTest, CaptureSnapshotHasNoLockedFlashByDefault) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  auto rect = app.document().document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());
  app.setSelection(*rect);

  const SelectionChromeSnapshot snapshot = OverlayRenderer::captureChromeSnapshot(
      std::span<const svg::SVGElement>(app.selectedElements()), std::nullopt, Transform2d());

  EXPECT_FALSE(snapshot.lockedFlash.has_value());
}

// An active locked-rejection flash captures the rejected element's outline plus
// its fade intensity, even when that element is NOT part of the selection (a
// locked click never selects).
TEST(OverlayRendererTest, CaptureSnapshotIncludesLockedFlashOutlineAndIntensity) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  auto rect = app.document().document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());

  // No selection - the locked element is flashed, not selected.
  const LockedRejectionFlashInput flashInput{.element = *rect, .intensity = 0.6f};
  const SelectionChromeSnapshot snapshot = OverlayRenderer::captureChromeSnapshot(
      std::span<const svg::SVGElement>(), std::nullopt, Transform2d(), std::nullopt,
      std::span<const svg::SVGElement>(), std::nullopt, SelectionChromeDetail::Full, Transform2d(),
      flashInput);

  EXPECT_TRUE(snapshot.paths.empty()) << "Locked element must not be drawn as a selection.";
  ASSERT_TRUE(snapshot.lockedFlash.has_value());
  EXPECT_FALSE(snapshot.lockedFlash->pathDoc.empty())
      << "Locked flash must carry the rejected element's outline.";
  EXPECT_FLOAT_EQ(snapshot.lockedFlash->intensity, 0.6f);
}

// A fully-faded flash (intensity 0) carries no payload - the outline must vanish
// at the end of the fade.
TEST(OverlayRendererTest, CaptureSnapshotDropsFullyFadedLockedFlash) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  auto rect = app.document().document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());

  const LockedRejectionFlashInput flashInput{.element = *rect, .intensity = 0.0f};
  const SelectionChromeSnapshot snapshot = OverlayRenderer::captureChromeSnapshot(
      std::span<const svg::SVGElement>(), std::nullopt, Transform2d(), std::nullopt,
      std::span<const svg::SVGElement>(), std::nullopt, SelectionChromeDetail::Full, Transform2d(),
      flashInput);

  EXPECT_FALSE(snapshot.lockedFlash.has_value());
}

// Render-level proof: drawing a snapshot with a locked flash paints a red
// outline (high red, low green/blue) around the flashed element, and the flash's
// alpha scales with intensity (brighter at 1.0 than at a lower intensity).
TEST(OverlayRendererTest, LockedFlashDrawsRedOutlineScaledByIntensity) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));
  app.document().document().setCanvasSize(200, 200);

  auto rect = app.document().document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());

  const Transform2d canvasFromDoc = app.document().document().canvasFromDocumentTransform();

  svg::RenderViewport viewport;
  viewport.size = Vector2d(200.0, 200.0);
  viewport.devicePixelRatio = 1.0;

  const auto drawFlashAtIntensity = [&](float intensity) {
    const LockedRejectionFlashInput flashInput{.element = *rect, .intensity = intensity};
    const SelectionChromeSnapshot snapshot = OverlayRenderer::captureChromeSnapshot(
        std::span<const svg::SVGElement>(), std::nullopt, canvasFromDoc, std::nullopt,
        std::span<const svg::SVGElement>(), std::nullopt, SelectionChromeDetail::Full,
        Transform2d(), flashInput);
    svg::Renderer overlayRenderer;
    overlayRenderer.beginFrame(viewport);
    OverlayRenderer::drawChromeFromSnapshot(overlayRenderer, snapshot);
    overlayRenderer.endFrame();
    return overlayRenderer.takeSnapshot();
  };

  const auto fullBitmap = drawFlashAtIntensity(1.0f);
  ASSERT_FALSE(fullBitmap.empty());

  const auto pixelAt = [](const auto& bitmap, int x, int y, int channel) -> std::uint8_t {
    const std::uint8_t* row = bitmap.pixels.data() + y * bitmap.rowBytes;
    return row[x * 4 + channel];
  };

  // The r1 rect spans doc x=[20,60], y=[30,80]; at 1:1 canvas scale its outline
  // runs along those edges. Scan the rectangle's outline band for a red flash
  // pixel: red channel high, green and blue low, alpha present.
  int maxRedAlphaFull = 0;
  for (int y = 28; y <= 82; ++y) {
    for (int x = 18; x <= 62; ++x) {
      const bool onOutline = x <= 22 || x >= 58 || y <= 32 || y >= 78;
      if (!onOutline) {
        continue;
      }
      const std::uint8_t r = pixelAt(fullBitmap, x, y, 0);
      const std::uint8_t g = pixelAt(fullBitmap, x, y, 1);
      const std::uint8_t b = pixelAt(fullBitmap, x, y, 2);
      const std::uint8_t a = pixelAt(fullBitmap, x, y, 3);
      if (r > 180 && g < 90 && b < 90 && a > 0) {
        maxRedAlphaFull = std::max<int>(maxRedAlphaFull, a);
      }
    }
  }
  EXPECT_GT(maxRedAlphaFull, 0) << "No red flash outline pixel found at intensity 1.0.";

  // A dimmer flash should produce a lower peak alpha on the same outline band.
  const auto dimBitmap = drawFlashAtIntensity(0.3f);
  ASSERT_FALSE(dimBitmap.empty());
  int maxRedAlphaDim = 0;
  for (int y = 28; y <= 82; ++y) {
    for (int x = 18; x <= 62; ++x) {
      const bool onOutline = x <= 22 || x >= 58 || y <= 32 || y >= 78;
      if (!onOutline) {
        continue;
      }
      const std::uint8_t r = pixelAt(dimBitmap, x, y, 0);
      const std::uint8_t g = pixelAt(dimBitmap, x, y, 1);
      const std::uint8_t b = pixelAt(dimBitmap, x, y, 2);
      const std::uint8_t a = pixelAt(dimBitmap, x, y, 3);
      if (r > 120 && g < 90 && b < 90 && a > 0) {
        maxRedAlphaDim = std::max<int>(maxRedAlphaDim, a);
      }
    }
  }
  EXPECT_GT(maxRedAlphaDim, 0) << "No red flash outline pixel found at intensity 0.3.";
  EXPECT_LT(maxRedAlphaDim, maxRedAlphaFull)
      << "Dimmer flash (0.3) should paint a lower peak alpha than a full flash (1.0).";
}

TEST(OverlayRendererTest, TextSelectionCapturesBoundsAndTransformHandles) {
  constexpr std::string_view kTextSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">
           <text id="t" x="50" y="80" font-size="20" font-family="sans-serif">Hello</text>
         </svg>)";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTextSvg));
  auto text = app.document().document().querySelector("#t");
  ASSERT_TRUE(text.has_value());
  app.setSelection(*text);

  const SelectionChromeSnapshot snapshot = OverlayRenderer::captureChromeSnapshot(
      std::span<const svg::SVGElement>(app.selectedElements()), std::nullopt, Transform2d());

  // Selected text gets the same overlay rectangle + drag/rotate handles as
  // shapes, driven by its laid-out ink bounds.
  ASSERT_EQ(snapshot.aabbsDoc.size(), 1u);
  EXPECT_GT(snapshot.aabbsDoc[0].size().x, 0.0);
  EXPECT_NEAR(snapshot.aabbsDoc[0].topLeft.x, 50.0, 3.0);
  EXPECT_LT(snapshot.aabbsDoc[0].topLeft.y, 80.0);
  EXPECT_FALSE(snapshot.handleBoxesDoc.empty());
}

TEST(OverlayRendererTest, TextFrameHandlesMatchSelectHandleSizeAtTwoXScale) {
  SelectionChromeSnapshot snapshot;
  snapshot.canvasFromDoc = Transform2d::Scale(2.0);
  snapshot.selectionStrokeWidthWorld = 1.25;
  snapshot.textFrameCornersDoc = std::array<Vector2d, 4>{
      Vector2d(20.0, 20.0), Vector2d(40.0, 20.0), Vector2d(40.0, 40.0), Vector2d(20.0, 40.0)};

  const svg::RendererBitmap bitmap = DrawSnapshot(snapshot);
  ASSERT_FALSE(bitmap.empty());
  const auto alphaAt = [&](int x, int y) {
    const std::uint8_t* row = bitmap.pixels.data() + y * bitmap.rowBytes;
    return row[x * 4 + 3];
  };

  // Top-left corner maps to (40,40). A 9px select-style handle covers the
  // near diagonal but not a point seven pixels out. The old text-specific
  // DPR calculation produced an 18px handle and covered both.
  EXPECT_GT(alphaAt(43, 43), 0);
  EXPECT_EQ(alphaAt(47, 47), 0);
}

TEST(OverlayRendererTest, ZeroOpacityPointFrameDrawsNoFrameOrHandles) {
  SelectionChromeSnapshot snapshot;
  snapshot.canvasFromDoc = Transform2d();
  snapshot.selectionStrokeWidthWorld = 1.25;
  snapshot.textFrameCornersDoc = std::array<Vector2d, 4>{
      Vector2d(20.0, 20.0), Vector2d(80.0, 20.0), Vector2d(80.0, 80.0), Vector2d(20.0, 80.0)};
  snapshot.textFrameOpacity = 0.0f;

  EXPECT_FALSE(HasAnyNonTransparentPixel(DrawSnapshot(snapshot)));
}

TEST(OverlayRendererTest, TextSelectionCapturesBaselineUnderlay) {
  constexpr std::string_view kTextSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">
           <text id="t" x="50" y="80" font-size="20" font-family="sans-serif">Hello</text>
         </svg>)";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTextSvg));
  auto text = app.document().document().querySelector("#t");
  ASSERT_TRUE(text.has_value());
  app.setSelection(*text);

  const SelectionChromeSnapshot snapshot = OverlayRenderer::captureChromeSnapshot(
      std::span<const svg::SVGElement>(app.selectedElements()), std::nullopt, Transform2d());

  // One baseline segment for the single line: it runs along the y="80"
  // baseline starting at the x="50" pen position, spanning the glyph run.
  ASSERT_EQ(snapshot.textBaselinesDoc.size(), 1u);
  EXPECT_NEAR(snapshot.textBaselinesDoc[0].startDoc.y, 80.0, 1.0);
  EXPECT_NEAR(snapshot.textBaselinesDoc[0].endDoc.y, 80.0, 1.0);
  EXPECT_NEAR(snapshot.textBaselinesDoc[0].startDoc.x, 50.0, 3.0);
  EXPECT_GT(snapshot.textBaselinesDoc[0].endDoc.x, snapshot.textBaselinesDoc[0].startDoc.x + 10.0);
}

TEST(OverlayRendererTest, MultiLineTextCapturesOneBaselinePerLine) {
  constexpr std::string_view kTextSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">
           <text id="t" x="50" y="80" font-size="20" font-family="sans-serif">
             <tspan x="50">One</tspan><tspan x="50" dy="24">Two</tspan>
           </text>
         </svg>)";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTextSvg));
  auto text = app.document().document().querySelector("#t");
  ASSERT_TRUE(text.has_value());
  app.setSelection(*text);

  const SelectionChromeSnapshot snapshot = OverlayRenderer::captureChromeSnapshot(
      std::span<const svg::SVGElement>(app.selectedElements()), std::nullopt, Transform2d());

  ASSERT_EQ(snapshot.textBaselinesDoc.size(), 2u);
  EXPECT_NEAR(snapshot.textBaselinesDoc[0].startDoc.y, 80.0, 1.0);
  EXPECT_NEAR(snapshot.textBaselinesDoc[1].startDoc.y, 104.0, 1.0);
}

TEST(OverlayRendererTest, TextBoxDragPreviewDrawsFrameBaselineAndIbeamDistinctFromMarquee) {
  // Pure pushed-state chrome: no document registry reads are involved, so
  // build the snapshot directly the way RenderCoordinator stamps it.
  SelectionChromeSnapshot snapshot;
  snapshot.canvasFromDoc = Transform2d();
  snapshot.selectionStrokeWidthWorld = 1.5;
  snapshot.hoverStrokeWidthWorld = 1.5;
  snapshot.marqueeStrokeWidthWorld = 1.5;
  snapshot.textBoxDragPreviewDoc = SelectionChromeSnapshot::TextBoxDragPreview{
      .boxDoc = Box2d(Vector2d(40.0, 40.0), Vector2d(160.0, 140.0)),
      .baselineStartDoc = Vector2d(45.0, 72.0),
      .baselineEndDoc = Vector2d(155.0, 72.0),
      .ibeamTopDoc = Vector2d(45.0, 43.0),
      .ibeamBottomDoc = Vector2d(45.0, 80.0),
  };

  svg::Renderer renderer;
  svg::RenderViewport viewport;
  viewport.size = Vector2d(200.0, 200.0);
  viewport.devicePixelRatio = 1.0;
  renderer.beginFrame(viewport);
  OverlayRenderer::drawChromeFromSnapshot(renderer, snapshot);
  renderer.endFrame();
  const auto bitmap = renderer.takeSnapshot();
  ASSERT_FALSE(bitmap.empty());

  const auto pixelAt = [&](int x, int y, int channel) -> std::uint8_t {
    const std::uint8_t* row = bitmap.pixels.data() + y * bitmap.rowBytes;
    return row[x * 4 + channel];
  };
  const auto isCyanAt = [&](int x, int y) {
    return pixelAt(x, y, 2) > 150 && pixelAt(x, y, 1) > 120 && pixelAt(x, y, 0) < 110 &&
           pixelAt(x, y, 3) > 0;
  };
  const auto anyCyanNear = [&](int x, int y) {
    for (int dy = -2; dy <= 2; ++dy) {
      for (int dx = -2; dx <= 2; ++dx) {
        if (isCyanAt(x + dx, y + dy)) {
          return true;
        }
      }
    }
    return false;
  };

  // Frame edges (crisp cyan stroke, no marquee-style fill).
  EXPECT_TRUE(anyCyanNear(100, 40)) << "top frame edge missing";
  EXPECT_TRUE(anyCyanNear(100, 140)) << "bottom frame edge missing";
  EXPECT_TRUE(anyCyanNear(40, 90)) << "left frame edge missing";
  EXPECT_TRUE(anyCyanNear(160, 90)) << "right frame edge missing";

  // First-baseline guidance segment.
  EXPECT_TRUE(anyCyanNear(100, 72)) << "baseline indicator missing";

  // I-beam bar at the future caret position.
  EXPECT_TRUE(anyCyanNear(45, 60)) << "I-beam bar missing";

  // Distinct from the marquee: the box interior stays unfilled. Sample a
  // point away from the baseline and I-beam.
  EXPECT_EQ(pixelAt(120, 110, 3), 0)
      << "drag preview interior must not carry the marquee's translucent fill";
}

}  // namespace
}  // namespace donner::editor
