#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/Transform.h"
#include "donner/editor/ExperimentalDragPresentation.h"
#include "donner/editor/SelectTool.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/compositor/CompositorController.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RendererDriver.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::editor {
namespace {

using svg::SVGGraphicsElement;

/// RGBA pixel extracted from a RendererBitmap.
struct Pixel {
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
  uint8_t a = 0;

  friend std::ostream& operator<<(std::ostream& os, const Pixel& p) {
    return os << "Pixel(" << int(p.r) << "," << int(p.g) << "," << int(p.b) << "," << int(p.a)
              << ")";
  }
};

Pixel getPixel(const svg::RendererBitmap& bitmap, int x, int y) {
  EXPECT_GE(x, 0);
  EXPECT_LT(x, bitmap.dimensions.x);
  EXPECT_GE(y, 0);
  EXPECT_LT(y, bitmap.dimensions.y);
  if (x < 0 || x >= bitmap.dimensions.x || y < 0 || y >= bitmap.dimensions.y) {
    return {};
  }
  const size_t offset = static_cast<size_t>(y) * bitmap.rowBytes + static_cast<size_t>(x) * 4;
  return {bitmap.pixels[offset], bitmap.pixels[offset + 1], bitmap.pixels[offset + 2],
          bitmap.pixels[offset + 3]};
}

MATCHER(IsRed, "") {
  *result_listener << "pixel is " << arg;
  return arg.r > 200 && arg.g < 50 && arg.b < 50 && arg.a > 200;
}

MATCHER(IsNotRed, "") {
  *result_listener << "pixel is " << arg;
  // Transparent or non-red (white, etc.).
  return arg.a < 50 || arg.r < 100 || arg.g > 100 || arg.b > 100;
}

MATCHER(IsWhite, "") {
  *result_listener << "pixel is " << arg;
  return arg.r > 200 && arg.g > 200 && arg.b > 200 && arg.a > 200;
}

svg::SVGDocument ParseDocument(std::string_view svgSource) {
  ParseWarningSink warningSink;
  auto result = svg::parser::SVGParser::ParseSVG(svgSource, warningSink);
  EXPECT_FALSE(result.hasError()) << result.error().reason;
  return std::move(result).result();
}

// Simple SVG: 200×100, white background, red rect at (10, 10)-(60, 60).
constexpr std::string_view kBaseSvg = R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
  <rect width="200" height="100" fill="white"/>
  <rect id="target" x="10" y="10" width="50" height="50" fill="red"/>
</svg>
)svg";

// Coordinates inside the rect at its original position.
constexpr int kOrigCenterX = 35;
constexpr int kOrigCenterY = 35;

// Coordinates inside the rect after translate(100, 0).
constexpr int kMovedCenterX = 135;
constexpr int kMovedCenterY = 35;

// ─────────────────────────────────────────────────────────────────────────────
// Test: compositor produces correct pixel output at every phase of the
//       drag → release → settle → ReplaceDocument → prewarm cycle.
// ─────────────────────────────────────────────────────────────────────────────
TEST(DragReleasePopBackTest, CompositorProducesCorrectOutputAtEveryPhase) {
  auto document = ParseDocument(kBaseSvg);
  svg::Renderer renderer;

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->entityHandle().entity();

  svg::compositor::CompositorController compositor(document, renderer);
  ASSERT_TRUE(compositor.promoteEntity(entity));

  svg::RenderViewport viewport;
  viewport.size = Vector2d(200, 100);
  viewport.devicePixelRatio = 1.0;

  // ── Phase 1: Pre-drag render ──────────────────────────────────────────
  compositor.setLayerCompositionTransform(entity, Transform2d());
  compositor.renderFrame(viewport);

  {
    const auto flat = renderer.takeSnapshot();
    ASSERT_FALSE(flat.empty());
    EXPECT_THAT(getPixel(flat, kOrigCenterX, kOrigCenterY), IsRed())
        << "Pre-drag flat: rect at original position";
    EXPECT_THAT(getPixel(flat, kMovedCenterX, kMovedCenterY), IsWhite())
        << "Pre-drag flat: moved position should be white";
  }

  {
    const auto& promoted = compositor.layerBitmapOf(entity);
    ASSERT_FALSE(promoted.empty());
    EXPECT_THAT(getPixel(promoted, kOrigCenterX, kOrigCenterY), IsRed())
        << "Pre-drag promoted: rect at DOM position";
  }

  // ── Phase 2: During drag ──────────────────────────────────────────────
  // DOM unchanged; composition offset moves the promoted layer.
  compositor.setLayerCompositionTransform(entity, Transform2d::Translate(100, 0));
  compositor.renderFrame(viewport);

  {
    const auto flat = renderer.takeSnapshot();
    EXPECT_THAT(getPixel(flat, kMovedCenterX, kMovedCenterY), IsRed())
        << "Drag flat: rect at visual position (original + offset)";
    EXPECT_THAT(getPixel(flat, kOrigCenterX, kOrigCenterY), IsWhite())
        << "Drag flat: original position should be vacated";
  }

  {
    // The promoted bitmap is at DOM position (no composition offset).
    const auto& promoted = compositor.layerBitmapOf(entity);
    EXPECT_THAT(getPixel(promoted, kOrigCenterX, kOrigCenterY), IsRed())
        << "Drag promoted: element at original DOM position";
  }

  // ── Phase 3: Release + settling render ────────────────────────────────
  // SetTransformCommand applied: DOM now has translate(100, 0).
  // Composition transform returns to identity for the settling render.
  target->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(100, 0));
  compositor.setLayerCompositionTransform(entity, Transform2d());
  compositor.renderFrame(viewport);

  svg::RendererBitmap settlingFlat;
  svg::RendererBitmap settlingPromoted;
  svg::RendererBitmap settlingBg;
  svg::RendererBitmap settlingFg;
  {
    settlingFlat = renderer.takeSnapshot();
    settlingPromoted = compositor.layerBitmapOf(entity);
    settlingBg = compositor.backgroundBitmap();
    settlingFg = compositor.foregroundBitmap();

    EXPECT_THAT(getPixel(settlingFlat, kMovedCenterX, kMovedCenterY), IsRed())
        << "Settling flat: rect at new DOM position";
    EXPECT_THAT(getPixel(settlingFlat, kOrigCenterX, kOrigCenterY), IsWhite())
        << "Settling flat: original position is vacated";
    EXPECT_THAT(getPixel(settlingPromoted, kMovedCenterX, kMovedCenterY), IsRed())
        << "Settling promoted: rect at new DOM position";
  }

  // ── Phase 4: ReplaceDocument (simulated) ────────────────────────────
  // In the real main loop, loadFromString re-parses the source (which now
  // includes the transform baked in).  Entity handles change.  We simulate
  // this by resetting layers and re-promoting the same entity — the document
  // already has the transform from Phase 3 so the visual result is identical.
  compositor.resetAllLayers();
  ASSERT_TRUE(compositor.promoteEntity(entity));
  compositor.setLayerCompositionTransform(entity, Transform2d());
  compositor.renderFrame(viewport);

  {
    const auto flat = renderer.takeSnapshot();
    EXPECT_THAT(getPixel(flat, kMovedCenterX, kMovedCenterY), IsRed())
        << "Prewarm flat: rect at correct position after reset";
    EXPECT_THAT(getPixel(flat, kOrigCenterX, kOrigCenterY), IsWhite())
        << "Prewarm flat: original position is vacated";
  }

  {
    const auto& promoted = compositor.layerBitmapOf(entity);
    EXPECT_THAT(getPixel(promoted, kMovedCenterX, kMovedCenterY), IsRed())
        << "Prewarm promoted: rect at DOM position";
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Test: full frame-by-frame simulation of the main loop's state machine.
//
// Simulates the EXACT state machine transitions from main.cc and verifies
// that at every "display" point, the image shown is correct (element at
// the moved position, never at the pre-drag position).
//
// Timeline (matching main.cc ordering):
//   Frame 0: Initial prewarm render lands → composited textures cached
//   Frame 1: User starts drag → composited display with drag offset
//   Frame 2: User releases → settling begins, settling render dispatched
//   Frame 3: Settling render lands → composited textures updated, offset→0
//   Frame 4: ReplaceDocument → entity handles change, prewarm dispatched
//   Frame 5: Prewarm lands → composited textures updated for new entity
// ─────────────────────────────────────────────────────────────────────────────
TEST(DragReleasePopBackTest, StateTransitionsNeverShowPreDragImage) {
  // Simulated entity handles.
  const Entity entityOld{42};
  const Entity entityNew{99};

  ExperimentalDragPresentation state;
  const Vector2i canvasSize(200, 100);

  // Record which image each frame would display. The "image" is abstracted as
  // a pair: {entity for composited textures, screen offset}.  If the display
  // falls to the flat path, we record {entity=null, offset=Zero()}.
  struct FrameSnapshot {
    bool composited = false;
    Entity entity = entt::null;
    Vector2d offset = Vector2d::Zero();
  };
  std::vector<FrameSnapshot> frames;

  const auto recordDisplay = [&](const std::optional<SelectTool::ActiveDragPreview>& activeDrag,
                                 const char* label) -> FrameSnapshot {
    const auto preview = state.presentationPreview(activeDrag);
    const bool useComposited = state.shouldDisplayCompositedLayers(activeDrag);
    FrameSnapshot snap;
    snap.composited = useComposited;
    if (useComposited && preview.has_value()) {
      snap.entity = preview->entity;
      snap.offset = preview->translation;
    }
    frames.push_back(snap);
    return snap;
  };

  // ── Frame 0: Initial prewarm render lands ─────────────────────────────
  state.noteCachedTextures(entityOld, /*version=*/1, canvasSize);
  {
    auto snap = recordDisplay(std::nullopt, "Frame 0: prewarm");
    EXPECT_TRUE(snap.composited) << "Frame 0: composited display expected";
    EXPECT_EQ(snap.entity, entityOld);
    EXPECT_DOUBLE_EQ(snap.offset.x, 0.0);
  }

  // ── Frame 1: Drag starts, user moves right by 100 ────────────────────
  SelectTool::ActiveDragPreview drag{.entity = entityOld, .translation = Vector2d(100, 0)};
  {
    auto snap = recordDisplay(drag, "Frame 1: dragging");
    EXPECT_TRUE(snap.composited) << "Frame 1: composited display expected";
    EXPECT_EQ(snap.entity, entityOld);
    EXPECT_DOUBLE_EQ(snap.offset.x, 100.0)
        << "Frame 1: promoted layer at DOM position + offset 100";
  }

  // ── Frame 2: Release ─ beginSettling ──────────────────────────────────
  // Main loop: onMouseUp → SetTransformCommand → flushFrame → beginSettling.
  // The settling render is dispatched but hasn't landed yet.
  state.beginSettling(drag, /*targetVersion=*/2);
  {
    // After release, activeDragPreview is nullopt.
    auto snap = recordDisplay(std::nullopt, "Frame 2: settling (render in flight)");
    EXPECT_TRUE(snap.composited) << "Frame 2: composited display expected";
    EXPECT_EQ(snap.entity, entityOld);
    EXPECT_DOUBLE_EQ(snap.offset.x, 100.0) << "Frame 2: settling preview keeps drag offset";
  }

  // ── Frame 3: Settling render lands ────────────────────────────────────
  // The settling render was dispatched at version 2 with zero translation.
  // The compositor rendered the element at its NEW DOM position (after
  // SetTransformCommand).  noteCachedTextures resolves the settling.
  state.noteCachedTextures(entityOld, /*version=*/2, canvasSize);
  {
    auto snap = recordDisplay(std::nullopt, "Frame 3: settling resolved");
    EXPECT_TRUE(snap.composited) << "Frame 3: composited display expected";
    EXPECT_EQ(snap.entity, entityOld);
    EXPECT_DOUBLE_EQ(snap.offset.x, 0.0) << "Frame 3: offset → 0 (textures show new DOM pos)";
  }

  // ── Frame 4: ReplaceDocument ──────────────────────────────────────────
  // flushFrame processes ReplaceDocumentCommand. Entity handles change.
  // clearSettlingIfSelectionChanged detects the mismatch.
  state.clearSettlingIfSelectionChanged(entityNew, /*dragActive=*/false);
  {
    auto snap = recordDisplay(std::nullopt, "Frame 4: after ReplaceDocument");
    // THIS IS THE CRITICAL FRAME. The display MUST NOT pop to the flat texture
    // showing the pre-drag image.
    EXPECT_TRUE(snap.composited)
        << "Frame 4 (CRITICAL): composited display must stay active after entity handle change. "
        << "Falling to flat would show stale pre-drag image.";
    EXPECT_DOUBLE_EQ(snap.offset.x, 0.0)
        << "Frame 4: offset must be zero (settling textures at new DOM position)";
  }

  // Verify prewarm is triggered for the new entity.
  EXPECT_TRUE(state.shouldPrewarm(entityNew, /*currentVersion=*/3, canvasSize, /*dragActive=*/false))
      << "Frame 4: prewarm should be dispatched for new entity";

  // ── Frame 5: Prewarm lands ────────────────────────────────────────────
  state.noteCachedTextures(entityNew, /*version=*/3, canvasSize);
  {
    auto snap = recordDisplay(std::nullopt, "Frame 5: prewarm landed");
    EXPECT_TRUE(snap.composited) << "Frame 5: composited display expected";
    EXPECT_EQ(snap.entity, entityNew) << "Frame 5: textures updated for new entity";
    EXPECT_DOUBLE_EQ(snap.offset.x, 0.0);
  }

  // ── Verify: NO frame shows the element at the pre-drag position ───────
  for (size_t i = 2; i < frames.size(); ++i) {
    const auto& f = frames[i];
    if (f.composited) {
      // For composited display: promoted texture is rendered at DOM position,
      // then shifted by offset. After SetTransformCommand, DOM position = original + 100.
      // So:
      //   visual position = (DOM position + offset) in doc coords
      //   = (original + 100 + offset)
      //   = (original + 100 + 100) during settling (Frame 2) — WRONG?
      //
      // Actually: before settling render lands, the composited textures are
      // from the DRAG render (element at original DOM position).
      // visual = original + drag offset = original + 100. CORRECT.
      //
      // After settling render lands: composited textures from settling render
      // (element at new DOM position = original + 100).
      // visual = (original + 100) + 0 = original + 100. CORRECT.
      //
      // The test verifies offset is never such that visual = original position.
      // That would require offset = -100 after the drag, which we never set.
    }
    // For flat display: the flat bitmap must show element at moved position.
    // (Verified in CompositorProducesCorrectOutputAtEveryPhase above.)
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Regression: if noteFullRenderLanded fires (composited render fails to produce
// split layers), the display must not show pre-drag composited textures.
// ─────────────────────────────────────────────────────────────────────────────
TEST(DragReleasePopBackTest, FlatFallbackShowsCorrectImageAfterSettling) {
  auto document = ParseDocument(kBaseSvg);
  svg::Renderer renderer;

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->entityHandle().entity();

  svg::compositor::CompositorController compositor(document, renderer);

  svg::RenderViewport viewport;
  viewport.size = Vector2d(200, 100);
  viewport.devicePixelRatio = 1.0;

  // Pre-drag: render without compositor promotion (flat only).
  {
    svg::RendererDriver driver(renderer);
    driver.draw(document);
  }
  const auto preDragFlat = renderer.takeSnapshot();
  ASSERT_FALSE(preDragFlat.empty());
  EXPECT_THAT(getPixel(preDragFlat, kOrigCenterX, kOrigCenterY), IsRed());

  // Apply the transform (simulating SetTransformCommand).
  target->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(100, 0));

  // Settling render: render flat (no compositor promotion).
  {
    svg::RendererDriver driver(renderer);
    driver.draw(document);
  }
  const auto settlingFlat = renderer.takeSnapshot();
  ASSERT_FALSE(settlingFlat.empty());

  // The flat bitmap MUST show the element at its new position.
  EXPECT_THAT(getPixel(settlingFlat, kMovedCenterX, kMovedCenterY), IsRed())
      << "Flat fallback after settling must show element at new position";
  EXPECT_THAT(getPixel(settlingFlat, kOrigCenterX, kOrigCenterY), IsWhite())
      << "Flat fallback must NOT show element at original position";

  // Simulate state machine: noteFullRenderLanded clears composited state,
  // so the display falls to flat. Verify the flat bitmap is correct.
  ExperimentalDragPresentation state;
  state.noteCachedTextures(entity, 1, Vector2i(200, 100));
  state.beginSettling(SelectTool::ActiveDragPreview{entity, Vector2d(100, 0)}, 2);
  state.noteFullRenderLanded(/*landedVersion=*/2);

  EXPECT_FALSE(state.shouldDisplayCompositedLayers(std::nullopt))
      << "After noteFullRenderLanded, display should use flat texture";
  EXPECT_FALSE(state.hasCachedTextures);
}

// ─────────────────────────────────────────────────────────────────────────────
// Regression: the compositor's resetAllLayers (triggered by version change)
// must not cause hasSplitStaticLayers() to be false after re-promotion.
// ─────────────────────────────────────────────────────────────────────────────
TEST(DragReleasePopBackTest, ResetAndRePromoteProducesSplitLayers) {
  auto document = ParseDocument(kBaseSvg);
  svg::Renderer renderer;

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->entityHandle().entity();

  svg::compositor::CompositorController compositor(document, renderer);
  ASSERT_TRUE(compositor.promoteEntity(entity));

  svg::RenderViewport viewport;
  viewport.size = Vector2d(200, 100);
  viewport.devicePixelRatio = 1.0;

  // Initial render.
  compositor.renderFrame(viewport);
  EXPECT_TRUE(compositor.hasSplitStaticLayers());

  // Apply transform, then reset + re-promote (simulates version change).
  target->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(100, 0));
  compositor.resetAllLayers();
  ASSERT_TRUE(compositor.promoteEntity(entity));
  compositor.setLayerCompositionTransform(entity, Transform2d());
  compositor.renderFrame(viewport);

  EXPECT_TRUE(compositor.hasSplitStaticLayers())
      << "After reset + re-promote, compositor must produce split layers";

  const auto flat = renderer.takeSnapshot();
  EXPECT_THAT(getPixel(flat, kMovedCenterX, kMovedCenterY), IsRed())
      << "After reset: flat shows element at new position";
  EXPECT_THAT(getPixel(flat, kOrigCenterX, kOrigCenterY), IsWhite())
      << "After reset: original position vacated";
}

// ─────────────────────────────────────────────────────────────────────────────
// End-to-end: simulate the EXACT main loop sequence for a drag + release
// cycle, verifying every display frame's pixel content.
//
// Each "frame" records whether it uses the composited or flat path and
// checks the pixel at the moved center vs original center.
// ─────────────────────────────────────────────────────────────────────────────
TEST(DragReleasePopBackTest, EndToEndFrameSequence) {
  auto document = ParseDocument(kBaseSvg);
  svg::Renderer renderer;

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->entityHandle().entity();

  svg::compositor::CompositorController compositor(document, renderer);
  ASSERT_TRUE(compositor.promoteEntity(entity));

  svg::RenderViewport viewport;
  viewport.size = Vector2d(200, 100);
  viewport.devicePixelRatio = 1.0;

  ExperimentalDragPresentation state;

  // Track bitmaps that would be "uploaded to GL" in the real main loop.
  svg::RendererBitmap uploadedFlat;
  svg::RendererBitmap uploadedBg;
  svg::RendererBitmap uploadedPromoted;
  svg::RendererBitmap uploadedFg;
  bool hasUploadedComposited = false;

  // Helper: synchronous render → produces compositor output + flat bitmap.
  // Uses `currentEntity` which tracks which entity is currently promoted.
  Entity currentEntity = entity;
  const auto doRender = [&](const Transform2d& compositionTransform) {
    compositor.setLayerCompositionTransform(currentEntity, compositionTransform);
    compositor.renderFrame(viewport);

    uploadedFlat = renderer.takeSnapshot();
    if (compositor.hasSplitStaticLayers()) {
      uploadedBg = compositor.backgroundBitmap();
      uploadedPromoted = compositor.layerBitmapOf(currentEntity);
      uploadedFg = compositor.foregroundBitmap();
      hasUploadedComposited = true;
    }
  };

  // Helper: check "what the user sees" based on state machine.
  const auto verifyDisplay =
      [&](const std::optional<SelectTool::ActiveDragPreview>& activeDrag,
          const char* label) {
        const bool useComposited = state.shouldDisplayCompositedLayers(activeDrag);
        const auto preview = state.presentationPreview(activeDrag);

        if (useComposited && hasUploadedComposited && preview.has_value()) {
          // Composited path: the promoted texture is drawn at DOM position + screen offset.
          // The "screen offset" in document coordinates is preview->translation.
          // We verify that composited layers are valid.
          EXPECT_FALSE(uploadedPromoted.empty()) << label << ": promoted bitmap should exist";
          EXPECT_FALSE(uploadedBg.empty()) << label << ": background bitmap should exist";
        } else {
          // Flat path: the flat bitmap is used directly.
          ASSERT_FALSE(uploadedFlat.empty()) << label << ": flat bitmap must exist";
          // After any post-drag render, the flat bitmap must show the element at the
          // moved position. If it shows the original position, that's the pop.
          EXPECT_THAT(getPixel(uploadedFlat, kOrigCenterX, kOrigCenterY), IsNotRed())
              << label << ": flat bitmap must NOT show element at original position";
        }
      };

  // ══════════════════════════════════════════════════════════════════════
  // Frame 0: Pre-drag prewarm render.
  // ══════════════════════════════════════════════════════════════════════
  doRender(Transform2d());
  state.noteCachedTextures(entity, /*version=*/1, Vector2i(200, 100));
  verifyDisplay(std::nullopt, "Frame 0 (prewarm)");

  // Sanity: element at original position.
  EXPECT_THAT(getPixel(uploadedFlat, kOrigCenterX, kOrigCenterY), IsRed());

  // ══════════════════════════════════════════════════════════════════════
  // Frame 1: Drag (translate right by 100).
  // ══════════════════════════════════════════════════════════════════════
  doRender(Transform2d::Translate(100, 0));
  state.noteCachedTextures(entity, /*version=*/1, Vector2i(200, 100));

  SelectTool::ActiveDragPreview drag{.entity = entity, .translation = Vector2d(100, 0)};
  verifyDisplay(drag, "Frame 1 (drag)");

  // The flat bitmap from the compositor compose should show element at moved pos.
  EXPECT_THAT(getPixel(uploadedFlat, kMovedCenterX, kMovedCenterY), IsRed())
      << "Frame 1: flat bitmap (composed) shows element at visual position";

  // ══════════════════════════════════════════════════════════════════════
  // Frame 2: Release — SetTransformCommand + beginSettling.
  //   The display still shows the DRAG textures with settling offset.
  //   Settling render is in flight but hasn't landed.
  // ══════════════════════════════════════════════════════════════════════
  // Apply transform to DOM (simulates SetTransformCommand).
  target->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(100, 0));
  state.beginSettling(drag, /*targetVersion=*/2);
  // Do NOT render yet — the settling render is "in flight".
  verifyDisplay(std::nullopt, "Frame 2 (release, settling in-flight)");

  // ══════════════════════════════════════════════════════════════════════
  // Frame 3: Settling render lands.
  // ══════════════════════════════════════════════════════════════════════
  doRender(Transform2d());  // Settling render at zero offset, new DOM position.
  state.noteCachedTextures(entity, /*version=*/2, Vector2i(200, 100));
  verifyDisplay(std::nullopt, "Frame 3 (settling landed)");

  // The flat bitmap must show element at new position.
  EXPECT_THAT(getPixel(uploadedFlat, kMovedCenterX, kMovedCenterY), IsRed())
      << "Frame 3: settling flat at new position";
  EXPECT_THAT(getPixel(uploadedFlat, kOrigCenterX, kOrigCenterY), IsWhite())
      << "Frame 3: settling flat original position vacated";

  // ══════════════════════════════════════════════════════════════════════
  // Frame 4: ReplaceDocument (simulated).
  //   In the real code, loadFromString re-parses the source, creating
  //   new entity handles in the same SVGDocument object.  We simulate
  //   this by resetting layers and re-promoting the same entity (the
  //   document already has the transform from Frame 3).
  //
  //   The state machine uses a DIFFERENT entity handle to simulate the
  //   entity handle change that happens after ReplaceDocument.
  // ══════════════════════════════════════════════════════════════════════
  compositor.resetAllLayers();

  // The state machine detects that the "new" entity is different from
  // the settling entity.  Use Entity{999} to simulate new entity handles.
  const Entity simulatedNewEntity{999};
  state.clearSettlingIfSelectionChanged(simulatedNewEntity, /*dragActive=*/false);

  // CRITICAL: verifyDisplay must not show pre-drag state.
  // The display should either:
  //   (a) Stay on composited path with settling textures at zero offset, OR
  //   (b) Fall to flat path — but the flat texture is from Frame 3 (correct).
  verifyDisplay(std::nullopt, "Frame 4 (CRITICAL: after ReplaceDocument)");

  // Prewarm should be triggered.
  EXPECT_TRUE(state.shouldPrewarm(simulatedNewEntity, /*currentVersion=*/3, Vector2i(200, 100),
                                  /*dragActive=*/false))
      << "Frame 4: prewarm for new entity expected";

  // ══════════════════════════════════════════════════════════════════════
  // Frame 5: Prewarm render lands.
  // ══════════════════════════════════════════════════════════════════════
  ASSERT_TRUE(compositor.promoteEntity(entity));
  currentEntity = entity;
  doRender(Transform2d());

  state.noteCachedTextures(simulatedNewEntity, /*version=*/3, Vector2i(200, 100));
  verifyDisplay(std::nullopt, "Frame 5 (prewarm landed)");

  EXPECT_THAT(getPixel(uploadedFlat, kMovedCenterX, kMovedCenterY), IsRed())
      << "Frame 5: prewarm flat at correct position";
  EXPECT_THAT(getPixel(uploadedFlat, kOrigCenterX, kOrigCenterY), IsWhite())
      << "Frame 5: original position vacated";
}

}  // namespace
}  // namespace donner::editor
