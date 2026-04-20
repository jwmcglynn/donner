#include "donner/editor/OverlayRenderer.h"

#include "donner/base/Transform.h"
#include "donner/editor/EditorApp.h"
#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "gtest/gtest.h"

namespace donner::editor {
namespace {

constexpr std::string_view kTrivialSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">
         <rect id="r1" x="20" y="30" width="40" height="50" fill="red"/>
       </svg>)";

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

  // Brand-new renderer — never had `draw(document)` called on it. This
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
  EXPECT_TRUE(foundOpaquePixel) << "Overlay bitmap is completely transparent — chrome was "
                                   "never drawn";
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

  // Frame 1: nothing selected — overlay must still produce a (blank,
  // transparent) bitmap without crashing.
  runOverlayPass(std::nullopt);

  // Frame 2: select r1 — overlay produces chrome.
  app.setSelection(*r1);
  runOverlayPass(app.selectedElement());

  // Frame 3: clear the selection — overlay back to blank.
  app.setSelection(std::nullopt);
  runOverlayPass(app.selectedElement());

  // Frame 4: re-select r1 — should match frame 2's behavior.
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
  // size so canvasFromDoc is identity — that lets us assert directly
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
  // Probe a handful of points along each edge — at least one must
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
      << "Path outline / AABB did not appear at the *transformed* element location — the "
         "renderer is still treating the spline as world coordinates";

  // Sanity check that the *local* (un-translated) area is mostly
  // empty. A well-behaved overlay touches the pre-translate rect at
  // (10..90, 20..60) only if the bounding box happens to overlap
  // (which it doesn't for translate(60, 40)). Pick a spot that's
  // clearly outside the transformed AABB to confirm.
  EXPECT_EQ(alphaAt(20, 30), 0)
      << "Pixel at the *un-transformed* rect interior is non-empty — looks like the "
         "old local-space path is still being drawn";
}

// Regression for "dragging a shape moves the path outline in the
// *opposite* direction". Pre-fix, `DrawElementPathOutline` had an
// extraneous `.inverse()` on `geometry.elementFromWorld()`, so a
// rect translated by (+60, +40) had its path outline drawn at
// canvas (-60, -40). The `PathOutlineFollowsElementTransform` test
// above didn't catch the sign flip because it uses a plain rect
// whose AABB corners coincide with the outline corners — the AABB
// stroke lit up the expected pixels even when the outline was in
// the wrong place.
//
// This test uses a path that *doesn't* coincide with its AABB: an
// 'X' across the rect (top-left → bottom-right, plus top-right →
// bottom-left). The AABB is a rect (20..60 horizontal, 20..60
// vertical), but the outline lives only on the two diagonals. We
// assert that the outline's *midpoint* — which lies on neither
// AABB edge — gets stroke pixels at the transformed location.
TEST(OverlayRendererTest, PathOutlineDrawnAtTransformedLocationNotInverted) {
  // 40x40 "X" path at (20, 20) with translate(40, 30), so its
  // world-space bounds are (60, 50) to (100, 90) and the center
  // (where the two diagonals cross) is at (80, 70). The AABB
  // corners are (60, 50), (100, 50), (60, 90), (100, 90) — none of
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
  // square around that point — at least one must be non-zero because
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
      << "Pixel at the inverted-translate location is non-empty — overlay is drawing "
         "the path at the sign-flipped transform location.";
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
  // rect's outline — at least one corner pixel from each rect should
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

// The selection AABB moved to the ImGui draw list. The overlay must
// stay path-only or the editor will double-draw the combined rect on
// top of the immediate-mode chrome.
TEST(OverlayRendererTest, MultiSelectDoesNotDrawCombinedAabbInOverlay) {
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
  // (20..180, 20..180). The corner at (180, 20) is INSIDE that
  // combined rect but OUTSIDE both selected rect outlines. If the
  // overlay still draws the combined AABB, this pixel cluster lights
  // up; if it draws only the per-element path outlines, it stays
  // transparent.
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
  EXPECT_FALSE(anyNonZeroNear(180, 20, 2))
      << "combined-AABB top-right corner still appears in the overlay";
  EXPECT_FALSE(anyNonZeroNear(20, 180, 2))
      << "combined-AABB bottom-left corner still appears in the overlay";
}

// Marquee chrome also moved to the ImGui draw list. The path overlay
// should stay transparent when there is no selected geometry.
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

  // Both inputs empty — should do nothing without crashing.
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

  // Reload the document — the previously-selected entity is now stale.
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

}  // namespace
}  // namespace donner::editor
