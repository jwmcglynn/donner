#include "donner/editor/EditorApp.h"

#include "donner/editor/ViewportGeometry.h"
#include "donner/svg/SVGPathElement.h"
#include "gtest/gtest.h"

namespace donner::editor {
namespace {

constexpr std::string_view kTrivialSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <rect id="r1" x="10" y="10" width="20" height="20" fill="red"/>
         <rect id="r2" x="50" y="50" width="20" height="20" fill="blue"/>
       </svg>)";

TEST(EditorAppTest, EmptyByDefault) {
  EditorApp app;
  EXPECT_FALSE(app.hasDocument());
  EXPECT_FALSE(app.hasSelection());
  EXPECT_FALSE(app.selectedElement().has_value());
  EXPECT_TRUE(app.structuredEditingEnabled());
}

TEST(EditorAppTest, StructuredEditingKillSwitchCanBeDisabled) {
  EditorApp app;
  ASSERT_TRUE(app.structuredEditingEnabled());

  app.setStructuredEditingEnabled(false);
  EXPECT_FALSE(app.structuredEditingEnabled());

  app.setStructuredEditingEnabled(true);
  EXPECT_TRUE(app.structuredEditingEnabled());
}

TEST(EditorAppTest, LoadFromStringPopulatesDocument) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));
  EXPECT_TRUE(app.hasDocument());
  EXPECT_FALSE(app.hasSelection());

  EXPECT_TRUE(app.document().document().querySelector("#r1").has_value());
  EXPECT_TRUE(app.document().document().querySelector("#r2").has_value());
}

TEST(EditorAppTest, LoadFromStringClearsExistingSelection) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  auto rect = app.document().document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());
  app.setSelection(*rect);
  EXPECT_TRUE(app.hasSelection());

  ASSERT_TRUE(app.loadFromString(kTrivialSvg));
  EXPECT_FALSE(app.hasSelection());
}

TEST(EditorAppTest, HitTestReturnsTopElementAtPoint) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  // Inside r1 (10,10 → 30,30).
  auto inR1 = app.hitTest(Vector2d(15.0, 15.0));
  ASSERT_TRUE(inR1.has_value());
  EXPECT_EQ(inR1->id(), "r1");

  // Inside r2 (50,50 → 70,70).
  auto inR2 = app.hitTest(Vector2d(60.0, 60.0));
  ASSERT_TRUE(inR2.has_value());
  EXPECT_EQ(inR2->id(), "r2");

  // Empty space outside both rects.
  auto miss = app.hitTest(Vector2d(80.0, 80.0));
  EXPECT_FALSE(miss.has_value());
}

TEST(EditorAppTest, HitTestReturnsNulloptWhenNoDocument) {
  EditorApp app;
  EXPECT_FALSE(app.hitTest(Vector2d(0.0, 0.0)).has_value());
}

TEST(EditorAppTest, ApplyMutationFlowsThroughDocument) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  auto rect = app.document().document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());

  app.applyMutation(
      EditorCommand::SetTransformCommand(*rect, Transform2d::Translate(Vector2d(99.0, 0.0))));

  EXPECT_EQ(app.document().queue().size(), 1u);
  EXPECT_TRUE(app.flushFrame());
  EXPECT_EQ(app.document().queue().size(), 0u);
}

TEST(EditorAppTest, SelectionSetAndClear) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  auto r1 = app.document().document().querySelector("#r1");
  ASSERT_TRUE(r1.has_value());

  app.setSelection(*r1);
  EXPECT_TRUE(app.hasSelection());
  ASSERT_TRUE(app.selectedElement().has_value());
  EXPECT_TRUE(*app.selectedElement() == *r1);
  EXPECT_EQ(app.selectedElements().size(), 1u);

  app.setSelection(std::nullopt);
  EXPECT_FALSE(app.hasSelection());
  EXPECT_TRUE(app.selectedElements().empty());
}

// Multi-select API (Milestone 4 of the editor UX design doc): a
// vector-shaped backing store for shift+click and marquee
// selections, with a single-element compatibility shim for back-compat
// callers like the source-pane highlight and the inspector readout.
TEST(EditorAppTest, MultiSelectionStoresEveryElement) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  auto r1 = app.document().document().querySelector("#r1");
  auto r2 = app.document().document().querySelector("#r2");
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());

  app.setSelection(std::vector<svg::SVGElement>{*r1, *r2});
  EXPECT_TRUE(app.hasSelection());
  ASSERT_EQ(app.selectedElements().size(), 2u);
  EXPECT_TRUE(app.selectedElements()[0] == *r1);
  EXPECT_TRUE(app.selectedElements()[1] == *r2);
  // Single-element compat: returns the *first* element.
  ASSERT_TRUE(app.selectedElement().has_value());
  EXPECT_TRUE(*app.selectedElement() == *r1);

  // clearSelection reads as the natural opposite of "set N elements".
  app.clearSelection();
  EXPECT_FALSE(app.hasSelection());
  EXPECT_TRUE(app.selectedElements().empty());
  EXPECT_FALSE(app.selectedElement().has_value());
}

TEST(EditorAppTest, ToggleInSelectionAddsThenRemoves) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  auto r1 = app.document().document().querySelector("#r1");
  auto r2 = app.document().document().querySelector("#r2");
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());

  app.toggleInSelection(*r1);
  EXPECT_EQ(app.selectedElements().size(), 1u);
  EXPECT_TRUE(app.selectedElements()[0] == *r1);

  app.toggleInSelection(*r2);
  EXPECT_EQ(app.selectedElements().size(), 2u);

  // Toggling an already-selected element removes it without
  // disturbing the other entries.
  app.toggleInSelection(*r1);
  EXPECT_EQ(app.selectedElements().size(), 1u);
  EXPECT_TRUE(app.selectedElements()[0] == *r2);

  // Re-toggling brings it back.
  app.toggleInSelection(*r1);
  EXPECT_EQ(app.selectedElements().size(), 2u);
}

TEST(EditorAppTest, AddToSelectionIsIdempotent) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  auto r1 = app.document().document().querySelector("#r1");
  ASSERT_TRUE(r1.has_value());

  app.addToSelection(*r1);
  app.addToSelection(*r1);
  app.addToSelection(*r1);
  EXPECT_EQ(app.selectedElements().size(), 1u);
}

TEST(EditorAppTest, SetAttributeOnSelectionQueuesEverySelectedElement) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  auto r1 = app.document().document().querySelector("#r1");
  auto r2 = app.document().document().querySelector("#r2");
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());
  app.setSelection(std::vector<svg::SVGElement>{*r1, *r2});

  ASSERT_TRUE(app.setAttributeOnSelection("fill", "#112233"));
  EXPECT_EQ(app.document().queue().size(), 2u);
  ASSERT_TRUE(app.flushFrame());

  auto updatedR1 = app.document().document().querySelector("#r1");
  auto updatedR2 = app.document().document().querySelector("#r2");
  ASSERT_TRUE(updatedR1.has_value());
  ASSERT_TRUE(updatedR2.has_value());
  EXPECT_EQ(updatedR1->getAttribute("fill"), "#112233");
  EXPECT_EQ(updatedR2->getAttribute("fill"), "#112233");
}

TEST(EditorAppTest, SetStylePropertyOnSelectionMergesIntoStyleAttribute) {
  constexpr std::string_view kStyledSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <rect id="r1" x="10" y="10" width="20" height="20" fill="red"
               style="stroke: blue; opacity: 0.5"/>
       </svg>)";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kStyledSvg));

  auto r1 = app.document().document().querySelector("#r1");
  ASSERT_TRUE(r1.has_value());
  app.setSelection(*r1);

  ASSERT_TRUE(app.setStylePropertyOnSelection("fill", "#112233"));
  EXPECT_EQ(app.document().queue().size(), 1u);
  ASSERT_TRUE(app.flushFrame());

  auto updatedR1 = app.document().document().querySelector("#r1");
  ASSERT_TRUE(updatedR1.has_value());
  EXPECT_EQ(updatedR1->getAttribute("fill"), "red");
  EXPECT_EQ(updatedR1->getAttribute("style"), "stroke: blue; opacity: 0.5; fill: #112233");
}

TEST(EditorAppTest, SetStylePropertyOnSelectionOverridesExistingStyleProperty) {
  constexpr std::string_view kStyledSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <rect id="r1" x="10" y="10" width="20" height="20" fill="red"
               style="fill: blue; stroke: black"/>
       </svg>)";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kStyledSvg));

  auto r1 = app.document().document().querySelector("#r1");
  ASSERT_TRUE(r1.has_value());
  app.setSelection(*r1);

  ASSERT_TRUE(app.setStylePropertyOnSelection("fill", "#112233"));
  ASSERT_TRUE(app.flushFrame());

  auto updatedR1 = app.document().document().querySelector("#r1");
  ASSERT_TRUE(updatedR1.has_value());
  EXPECT_EQ(updatedR1->getAttribute("style"), "stroke: black; fill: #112233");
}

TEST(EditorAppTest, SetStrokeWidthOnSelectionClampsNegativeValues) {
  constexpr std::string_view kStyledSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <rect id="r1" x="10" y="10" width="20" height="20" stroke-width="4"
               style="stroke: black; opacity: 0.5"/>
       </svg>)";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kStyledSvg));

  auto r1 = app.document().document().querySelector("#r1");
  ASSERT_TRUE(r1.has_value());
  app.setSelection(*r1);

  ASSERT_TRUE(app.setStrokeWidthOnSelection(-4.0));
  ASSERT_TRUE(app.flushFrame());

  auto updatedR1 = app.document().document().querySelector("#r1");
  ASSERT_TRUE(updatedR1.has_value());
  EXPECT_EQ(updatedR1->getAttribute("stroke-width"), "4");
  EXPECT_EQ(updatedR1->getAttribute("style"), "stroke: black; opacity: 0.5; stroke-width: 0");
}

TEST(EditorAppTest, SetStrokeWidthOnSelectionOverridesExistingStyleProperty) {
  constexpr std::string_view kStyledSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <rect id="r1" x="10" y="10" width="20" height="20" stroke-width="4"
               style="stroke-width: 9; stroke: black"/>
       </svg>)";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kStyledSvg));

  auto r1 = app.document().document().querySelector("#r1");
  ASSERT_TRUE(r1.has_value());
  app.setSelection(*r1);

  ASSERT_TRUE(app.setStrokeWidthOnSelection(2.5));
  ASSERT_TRUE(app.flushFrame());

  auto updatedR1 = app.document().document().querySelector("#r1");
  ASSERT_TRUE(updatedR1.has_value());
  EXPECT_EQ(updatedR1->getAttribute("stroke-width"), "4");
  EXPECT_EQ(updatedR1->getAttribute("style"), "stroke: black; stroke-width: 2.5");
}

TEST(EditorAppTest, SetActiveStrokeWidthClampsNegativeValues) {
  EditorApp app;

  app.setActiveStrokeWidth(-4.0);

  EXPECT_EQ(app.activePaintStyle().strokeWidth, 0.0);
}

TEST(EditorAppTest, PathOperationAvailabilityRequiresMultipleGeometryElements) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  EXPECT_FALSE(app.pathOperationAvailability(PathOperationKind::Union).canApply);

  auto r1 = app.document().document().querySelector("#r1");
  auto r2 = app.document().document().querySelector("#r2");
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());

  app.setSelection(*r1);
  EXPECT_FALSE(app.pathOperationAvailability(PathOperationKind::Union).canApply);

  app.setSelection(std::vector<svg::SVGElement>{*r1, *r2});
  EXPECT_TRUE(app.pathOperationAvailability(PathOperationKind::Union).canApply);
  EXPECT_FALSE(app.pathOperationAvailability(PathOperationKind::SubtractFront).canApply);
}

TEST(EditorAppTest, PathUnionReplacesSelectionWithBoundsPath) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  auto r1 = app.document().document().querySelector("#r1");
  auto r2 = app.document().document().querySelector("#r2");
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());
  app.setSelection(std::vector<svg::SVGElement>{*r1, *r2});

  ASSERT_TRUE(app.applyPathOperation(PathOperationKind::Union));
  ASSERT_EQ(app.selectedElements().size(), 1u);
  ASSERT_TRUE(app.selectedElements().front().isa<svg::SVGPathElement>());
  EXPECT_EQ(std::string_view(app.selectedElements().front().cast<svg::SVGPathElement>().d()),
            "M 10 10 L 70 10 L 70 70 L 10 70 Z");

  ASSERT_TRUE(app.flushFrame());
  EXPECT_FALSE(app.document().document().querySelector("#r1").has_value());
  EXPECT_FALSE(app.document().document().querySelector("#r2").has_value());
  auto result = app.document().document().querySelector("path");
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(result->isa<svg::SVGPathElement>());
  EXPECT_EQ(std::string_view(result->cast<svg::SVGPathElement>().d()),
            "M 10 10 L 70 10 L 70 70 L 10 70 Z");
  EXPECT_NE(app.document().document().source().find(R"(<path d="M 10 10 L 70 10)"),
            std::string_view::npos);
}

TEST(EditorAppTest, PathIntersectReplacesSelectionWithOverlapPath) {
  constexpr std::string_view kOverlappingSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <rect id="r1" x="10" y="10" width="40" height="40" fill="red"/>
         <rect id="r2" x="30" y="25" width="40" height="20" fill="blue"/>
       </svg>)";
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kOverlappingSvg));

  auto r1 = app.document().document().querySelector("#r1");
  auto r2 = app.document().document().querySelector("#r2");
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());
  app.setSelection(std::vector<svg::SVGElement>{*r1, *r2});

  ASSERT_TRUE(app.applyPathOperation(PathOperationKind::Intersect));
  ASSERT_TRUE(app.flushFrame());

  auto result = app.document().document().querySelector("path");
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(result->isa<svg::SVGPathElement>());
  EXPECT_EQ(std::string_view(result->cast<svg::SVGPathElement>().d()),
            "M 30 25 L 50 25 L 50 45 L 30 45 Z");
}

TEST(EditorAppTest, PathIntersectDisabledWhenBoundsDoNotOverlap) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  auto r1 = app.document().document().querySelector("#r1");
  auto r2 = app.document().document().querySelector("#r2");
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());
  app.setSelection(std::vector<svg::SVGElement>{*r1, *r2});

  EXPECT_FALSE(app.pathOperationAvailability(PathOperationKind::Intersect).canApply);
  EXPECT_FALSE(app.applyPathOperation(PathOperationKind::Intersect));
  EXPECT_EQ(app.document().queue().size(), 0u);
}

TEST(EditorAppTest, HitTestRectFindsAllIntersectingElements) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  // r1 lives at (10,10..30,30), r2 at (50,50..70,70). A marquee
  // covering the full document grabs both.
  auto bothHits = app.hitTestRect(Box2d::FromXYWH(0.0, 0.0, 100.0, 100.0));
  EXPECT_EQ(bothHits.size(), 2u);

  // A marquee that only overlaps r1 returns just r1.
  auto r1Only = app.hitTestRect(Box2d::FromXYWH(5.0, 5.0, 20.0, 20.0));
  ASSERT_EQ(r1Only.size(), 1u);
  EXPECT_EQ(r1Only[0].id(), "r1");

  // A marquee that misses both returns empty.
  auto noHits = app.hitTestRect(Box2d::FromXYWH(80.0, 80.0, 5.0, 5.0));
  EXPECT_TRUE(noHits.empty());

  // Edge contact (marquee touches r1's edge) counts as intersection.
  auto edgeHits = app.hitTestRect(Box2d::FromXYWH(30.0, 30.0, 5.0, 5.0));
  ASSERT_EQ(edgeHits.size(), 1u);
  EXPECT_EQ(edgeHits[0].id(), "r1");
}

TEST(EditorAppTest, HitTestRectSkipsXmlTextNodeChildren) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
           <text id="label" x="5" y="15">Hello</text>
           <rect id="r1" x="10" y="10" width="20" height="20" fill="red"/>
         </svg>)"));

  auto hits = app.hitTestRect(Box2d::FromXYWH(0.0, 0.0, 100.0, 100.0));
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0].id(), "r1");
}

TEST(EditorAppTest, HitTestRectReturnsEmptyWithoutDocument) {
  EditorApp app;
  EXPECT_TRUE(app.hitTestRect(Box2d::FromXYWH(0.0, 0.0, 100.0, 100.0)).empty());
}

TEST(EditorAppTest, SyncDirtyFromSourceClearsWhenTextReturnsToCleanBaseline) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  app.setCleanSourceText(kTrivialSvg);
  EXPECT_FALSE(app.isDirty());

  const std::string edited = std::string(kTrivialSvg) + "\n<!-- edit -->\n";
  app.syncDirtyFromSource(edited);
  EXPECT_TRUE(app.isDirty());

  app.syncDirtyFromSource(kTrivialSvg);
  EXPECT_FALSE(app.isDirty());
}

TEST(EditorAppTest, RevertToCleanSourceReloadsLastSavedDocument) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));
  app.setCleanSourceText(kTrivialSvg);

  auto rect = app.document().document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());
  app.setSelection(*rect);

  constexpr std::string_view kEditedSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <circle id="replacement" cx="25" cy="25" r="10" fill="green"/>
       </svg>)";
  app.applyMutation(EditorCommand::ReplaceDocumentCommand(std::string(kEditedSvg)));
  ASSERT_TRUE(app.flushFrame());
  EXPECT_TRUE(app.isDirty());
  EXPECT_FALSE(app.document().document().querySelector("#r1").has_value());
  auto replacement = app.document().document().querySelector("#replacement");
  ASSERT_TRUE(replacement.has_value());
  app.setSelection(*replacement);
  EXPECT_TRUE(app.hasSelection());

  ASSERT_TRUE(app.revertToCleanSource());
  EXPECT_FALSE(app.isDirty());
  EXPECT_FALSE(app.hasSelection());
  EXPECT_TRUE(app.document().document().querySelector("#r1").has_value());
  EXPECT_FALSE(app.document().document().querySelector("#replacement").has_value());
}

// Regression for the "scale is wrong, clicks land on the background" bug in
// the editor's main loop. Mirrors exactly the sequence main.cc runs each
// frame:
//   1. Load a document whose intrinsic viewBox differs from the editor pane.
//   2. Set the canvas size to the pane size (the renderer draws a
//      pane-sized bitmap that the user sees).
//   3. Snapshot `cachedDocViewBox` from the document transform the way the
//      render-request path does.
//   4. Build a `DrawingViewportLayout` with the full pane filled at zoom=1,
//      pan=0, and ask `screenToDocument` to map the pane center.
//
// The pane center must hit the center of the viewBox. `canvasFromDocument`
// bakes in the preserveAspectRatio scale + letterbox offset, so click math
// needs its inverse — leaving it un-inverted (or worse, using the old
// misnamed `documentFromCanvasTransform()`) is exactly the "I keep selecting
// the background when I'm trying to click on a letter path" failure mode.
TEST(EditorAppTest, CenterClickOnPaneHitsCenterOfDocumentViewBox) {
  // donner_splash.svg shape: viewBox 892x512 rendered into a ~square pane.
  constexpr std::string_view kViewBoxDoc =
      R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 892 512">
           <rect id="fill" x="0" y="0" width="892" height="512" fill="white"/>
           <rect id="target" x="436" y="246" width="20" height="20" fill="red"/>
         </svg>)";

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kViewBoxDoc));

  // Pane is 720x800 — narrower than the 892-wide viewBox, so with the
  // default preserveAspectRatio="xMidYMid meet" the rendered content is
  // vertically letterboxed inside the pane.
  constexpr int kPaneW = 720;
  constexpr int kPaneH = 800;
  app.document().document().setCanvasSize(kPaneW, kPaneH);

  // Auto-fit may shrink the canvas to preserve the document's aspect
  // ratio — the renderer draws at this size, not at the pane size.
  // This is what main.cc re-reads as `currentCanvasSize` after calling
  // `setCanvasSize`.
  const Vector2i renderedCanvas = app.document().document().canvasSize();
  ASSERT_GT(renderedCanvas.x, 0);
  ASSERT_GT(renderedCanvas.y, 0);

  // Same snapshot code as main.cc at render-request time. `canvasFromDoc`
  // maps viewBox points to canvas pixels (see
  // `SVGDocument.CanvasFromDocumentTransformScaling`), so click math wants
  // the reverse direction.
  const Transform2d docFromCanvas =
      app.document().document().canvasFromDocumentTransform().inverse();
  const Box2d canvasBox = Box2d::FromXYWH(0.0, 0.0, static_cast<double>(renderedCanvas.x),
                                          static_cast<double>(renderedCanvas.y));
  const Box2d cachedDocViewBox = docFromCanvas.transformBox(canvasBox);

  // Pane lives at screen origin (0, 0). zoom=1, pan=0, so imageSize
  // matches the rendered canvas (720x413), vertically centered in the
  // 720x800 pane by `ComputeDrawingViewportLayout`.
  const DrawingViewportLayout layout = ComputeDrawingViewportLayout(
      Vector2d(0.0, 0.0), Vector2d(kPaneW, kPaneH), Vector2d(renderedCanvas.x, renderedCanvas.y),
      Vector2d(0.0, 0.0), cachedDocViewBox);

  // Image should be vertically centered: origin y = (800-413)/2 = 193.5.
  EXPECT_DOUBLE_EQ(layout.imageOrigin.x, 0.0);
  EXPECT_DOUBLE_EQ(layout.imageOrigin.y, (kPaneH - renderedCanvas.y) / 2.0);

  // A click at the center of the pane must land at the center of the
  // viewBox (892/2, 512/2) = (446, 256) — which is inside #target.
  const auto center = layout.screenToDocument(Vector2d(kPaneW / 2.0, kPaneH / 2.0));
  ASSERT_TRUE(center.has_value());
  EXPECT_NEAR(center->x, 446.0, 1.0) << "center=" << *center;
  EXPECT_NEAR(center->y, 256.0, 1.0) << "center=" << *center;

  // And the same point should hit-test to #target (not the background
  // #fill rect).
  auto hit = app.hitTest(*center);
  ASSERT_TRUE(hit.has_value());
  EXPECT_EQ(hit->id(), "target");
}

}  // namespace
}  // namespace donner::editor
