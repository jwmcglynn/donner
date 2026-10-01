#include "donner/editor/FramePresentation.h"

#include "donner/editor/EditorApp.h"
#include "donner/editor/PresentedFrameComposer.h"
#include "donner/editor/tests/FramePresentationTestAccess.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace donner::editor {
namespace {

using testing::Eq;
using testing::IsFalse;
using testing::IsTrue;
using testing::Ne;
using testing::SizeIs;

constexpr std::string_view kRect =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
  <rect id="rect" x="10" y="10" width="20" height="20" fill="red"/></svg>)";

PresentationIdentity Identity(EditorApp& app, std::uint64_t captureId) {
  return PresentationIdentity{.captureId = captureId,
                              .documentGeneration = app.document().documentGeneration(),
                              .version = app.document().currentFrameVersion(),
                              .geometryRevision = app.document().nonTransformRevision(),
                              .fontResourceRevision = app.document().fontResourceRevision()};
}

std::shared_ptr<const CapturedPresentation> Capture(EditorApp& app, std::uint64_t id) {
  std::vector<Entity> independent;
  for (const auto& element : app.selectedElements()) {
    independent.push_back(element.unsafeEntityHandle().entity());
  }
  return CapturedPresentation::Capture(app.document().document(), Identity(app, id),
                                       app.selectedElements(), {}, independent);
}

GlTextureCache::TileView RectTile(Entity entity, double rasterX, double composeX) {
  GlTextureCache::TileView tile;
  tile.texture = 1;
  tile.id = "rect";
  tile.kind = RenderResult::CompositedTile::Kind::Layer;
  tile.layerEntity = entity;
  tile.canvasOffsetDoc = Vector2d(rasterX, 10.0);
  tile.bitmapDimsDoc = Vector2d(20.0, 20.0);
  tile.bitmapDimsPx = Vector2i(20, 20);
  tile.documentFromCachedDocument = Transform2d::Translate(composeX, 0.0);
  tile.dragTranslationDoc = Vector2d(composeX, 0.0);
  return tile;
}

FramePresentationInput Input(EditorApp& app, SelectTool& tool, std::uint64_t frameId) {
  FramePresentationInput input;
  input.frameId = frameId;
  input.viewport.documentViewBox = Box2d::FromXYWH(0.0, 0.0, 100.0, 100.0);
  input.viewport.paneSize = Vector2d(512.0, 512.0);
  input.viewport.zoom = 1.93;
  input.paneClipRect = Box2d(Vector2d::Zero(), input.viewport.paneSize);
  input.documentIdentity = Identity(app, 0);
  input.desired = tool.activeDragPreview();
  for (const auto& element : app.selectedElements()) {
    input.selection.push_back(element.unsafeEntityHandle().entity());
  }
  return input;
}

void ExpectRectAt(const FramePresentation& frame, double x) {
  ASSERT_THAT(frame.tiles(), SizeIs(1));
  ASSERT_THAT(frame.chrome().paths, SizeIs(1));
  EXPECT_THAT(frame.chrome().paths.front().pathDoc.bounds(),
              Eq(Box2d::FromXYWH(x, 10.0, 20.0, 20.0)));
  const auto& tile = frame.tiles().front();
  const auto quad = ComputePresentedTileQuad(
      PresentedFrameTileGeometry{.canvasOffsetDoc = tile.canvasOffsetDoc,
                                 .bitmapDimsDoc = tile.bitmapDimsDoc,
                                 .documentFromCachedDocument = tile.documentFromCachedDocument},
      frame.framebufferFromDocument(Vector2d(1.0, 1.0)), std::nullopt);
  ASSERT_THAT(quad, Ne(std::nullopt));
  EXPECT_NEAR(quad->topLeft.x, x * 1.93, 1e-8);
  EXPECT_NEAR(quad->topRight.x, (x + 20.0) * 1.93, 1e-8);
  EXPECT_NEAR(quad->topLeft.y, 10.0 * 1.93, 1e-8);
}

TEST(FramePresentationTest, SecondDragAndLateRasterShareOnePose) {
  EditorApp app;
  ASSERT_THAT(app.loadFromString(kRect), IsTrue());
  SelectTool tool;
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(35.0, 15.0), true);
  ASSERT_THAT(app.flushFrame(), IsTrue());
  const auto first = Capture(app, 1);
  ASSERT_THAT(first, Ne(nullptr));
  const Entity entity = first->selection().front();
  const auto stale = FramePresentationTestAccess::resources(first, {RectTile(entity, 10.0, 20.0)});
  tool.onMouseMove(app, Vector2d(65.0, 15.0), true);
  ASSERT_THAT(app.flushFrame(), IsTrue());
  tool.onMouseUp(app, Vector2d(65.0, 15.0));
  tool.onMouseDown(app, Vector2d(65.0, 15.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(70.0, 15.0), true);
  auto frame = FramePresentation::Build(stale, Input(app, tool, 1)).frame;
  ASSERT_THAT(frame, Ne(nullptr));
  EXPECT_THAT(frame->followsPointer(), IsTrue());
  ExpectRectAt(*frame, 65.0);

  ASSERT_THAT(app.flushFrame(), IsTrue());
  const auto second = Capture(app, 2);
  const auto rerasterized =
      FramePresentationTestAccess::resources(second, {RectTile(entity, 65.0, 0.0)});
  tool.onMouseMove(app, Vector2d(75.0, 15.0), true);
  const auto beforeArrival = FramePresentation::Build(stale, Input(app, tool, 2)).frame;
  const auto afterArrival = FramePresentation::Build(rerasterized, Input(app, tool, 3)).frame;
  ASSERT_THAT(beforeArrival, Ne(nullptr));
  ASSERT_THAT(afterArrival, Ne(nullptr));
  ExpectRectAt(*beforeArrival, 70.0);
  ExpectRectAt(*afterArrival, 70.0);
  EXPECT_THAT(afterArrival->identity().captureId, Eq(2u));
}

TEST(FramePresentationTest, FlattenedSelectionKeepsPixelsAndChromeAtCapturedPose) {
  EditorApp app;
  ASSERT_THAT(app.loadFromString(kRect), IsTrue());
  SelectTool tool;
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  const auto captured = Capture(app, 1);
  auto flattened = RectTile(captured->selection().front(), 10.0, 0.0);
  flattened.layerEntity = entt::null;
  flattened.kind = RenderResult::CompositedTile::Kind::Segment;
  const auto resources = FramePresentationTestAccess::resources(captured, {flattened});
  tool.onMouseMove(app, Vector2d(45.0, 15.0), true);
  const auto frame = FramePresentation::Build(resources, Input(app, tool, 1)).frame;
  ASSERT_THAT(frame, Ne(nullptr));
  EXPECT_THAT(frame->followsPointer(), IsFalse());
  ExpectRectAt(*frame, 10.0);
}

TEST(FramePresentationTest, DifferentCapturedSelectionCannotAnnotateTheRaster) {
  EditorApp app;
  ASSERT_THAT(app.loadFromString(kRect), IsTrue());
  SelectTool tool;
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  const auto captured = Capture(app, 1);
  const auto resources = FramePresentationTestAccess::resources(
      captured, {RectTile(captured->selection().front(), 10.0, 0.0)});
  auto input = Input(app, tool, 1);
  input.selection.clear();
  const auto frame = FramePresentation::Build(resources, input).frame;
  ASSERT_THAT(frame, Ne(nullptr));
  EXPECT_THAT(frame->hasSelectionGeometry(), IsFalse());
  EXPECT_THAT(frame->chrome().paths, testing::IsEmpty());
  EXPECT_THAT(frame->chrome().handleAnchorsDoc, testing::IsEmpty());
  EXPECT_THAT(frame->tiles(), SizeIs(1));
}

TEST(FramePresentationTest, PartialRasterCannotMoveAnUncapturedRegionIntoView) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kRect));
  SelectTool tool;
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  const auto capture = Capture(app, 1);
  auto partial = RectTile(capture->selection().front(), 10.0, 0.0);
  partial.bitmapDimsDoc.x = 10.0;
  partial.bitmapDimsPx.x = 10;
  const auto resources = FramePresentationTestAccess::resources(capture, {partial});
  tool.onMouseMove(app, Vector2d(25.0, 15.0), true);
  const auto frame = FramePresentation::Build(resources, Input(app, tool, 1)).frame;
  ASSERT_NE(frame, nullptr);
  EXPECT_FALSE(frame->followsPointer());
  EXPECT_EQ(frame->chrome().paths.front().pathDoc.bounds(),
            Box2d::FromXYWH(10.0, 10.0, 20.0, 20.0));
  EXPECT_TRUE(
      SamePresentationTransform(frame->tiles().front().documentFromCachedDocument, Transform2d()));
}

TEST(FramePresentationTest,
     RetainedDisplayedPoseSurvivesDeselectionAndRejectsIncompleteReplacement) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kRect));
  SelectTool tool;
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  const auto capture = Capture(app, 1);
  const Entity entity = capture->selection().front();
  const auto resources = FramePresentationTestAccess::resources(capture, {RectTile(entity, 10, 0)});
  tool.onMouseMove(app, Vector2d(35.0, 15.0), true);
  const auto moved = FramePresentation::Build(resources, Input(app, tool, 1)).frame;
  ASSERT_NE(moved, nullptr);
  ExpectRectAt(*moved, 30);
  auto deselected = Input(app, tool, 2);
  deselected.selection.clear();
  deselected.desired.reset();
  const auto retained = FramePresentation::Build(resources, deselected, nullptr, moved).frame;
  ASSERT_NE(retained, nullptr);
  EXPECT_TRUE(retained->chrome().paths.empty());
  EXPECT_EQ(retained->tiles().front().documentFromCachedDocument.translation(), Vector2d(20, 0));
  EXPECT_THAT(retained->overrides(), SizeIs(1));
  auto incomplete = RectTile(entity, 10, 0);
  incomplete.bitmapDimsDoc.x = 5;
  const auto cropped = FramePresentationTestAccess::resources(capture, {incomplete});
  deselected.frameId++;
  EXPECT_EQ(FramePresentation::Build(cropped, deselected, nullptr, retained).frame, nullptr);
}

TEST(FramePresentationTest, ZoomOutRequiresCompatibleCompleteOverview) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kRect));
  SelectTool tool;
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  const auto capture = Capture(app, 1);
  const auto tile = RectTile(capture->selection().front(), 10, 0);
  const PresentationCoverageDiagnostics coverage{
      .activeTilesViewportBounded = true,
      .activeRasterDocumentRect = Box2d::FromXYWH(0, 0, 50, 50)};
  const auto cropped = FramePresentationTestAccess::resources(capture, {tile}, coverage);
  const auto input = Input(app, tool, 1);
  EXPECT_EQ(FramePresentation::Build(cropped, input).frame, nullptr);
  const auto paired =
      FramePresentationTestAccess::resources(capture, {tile}, coverage, capture, {tile});
  const auto frame = FramePresentation::Build(paired, input).frame;
  ASSERT_NE(frame, nullptr);
  ExpectRectAt(*frame, 10);
  app.document().document().querySelector("#rect")->setAttribute("x", "60");
  const auto changed = Capture(app, 2);
  ASSERT_EQ(changed->identity().version, capture->identity().version);
  ASSERT_NE(changed->identity().documentRevision, capture->identity().documentRevision);
  const auto torn =
      FramePresentationTestAccess::resources(changed, {tile}, coverage, capture, {tile});
  EXPECT_EQ(FramePresentation::Build(torn, input).frame, nullptr);
}

TEST(FramePresentationTest, QueuedResizeSharesPathBoundsAndTileTransform) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kRect));
  app.setSelection(*app.document().document().querySelector("#rect"));
  SelectTool tool;
  tool.onMouseDown(app, Vector2d(30, 30), MouseModifiers{});
  const auto capture = Capture(app, 1);
  const auto resources = FramePresentationTestAccess::resources(
      capture, {RectTile(capture->selection().front(), 10, 0)});
  tool.onMouseMove(app, Vector2d(40, 50), true);
  ASSERT_TRUE(app.document().hasPendingMutations());
  auto input = Input(app, tool, 1);
  ASSERT_TRUE(input.desired.has_value());
  const auto frame = FramePresentation::Build(resources, input).frame;
  ASSERT_NE(frame, nullptr);
  ASSERT_TRUE(frame->followsPointer());
  const auto expected =
      input.desired->documentFromCachedDocument.transformBox(Box2d::FromXYWH(10, 10, 20, 20));
  EXPECT_EQ(frame->chrome().paths.front().pathDoc.bounds(), expected);
  const auto& tile = frame->tiles().front();
  EXPECT_EQ(tile.documentFromCachedDocument.transformBox(
                Box2d(tile.canvasOffsetDoc, tile.canvasOffsetDoc + tile.bitmapDimsDoc)),
            expected);
  ASSERT_TRUE(frame->chrome().orientedBoundsDoc.has_value());
  const auto& corners = frame->chrome().orientedBoundsDoc->cornersDoc;
  EXPECT_EQ(corners.front(), expected.topLeft);
  EXPECT_EQ(corners[2], expected.bottomRight);
}

TEST(FramePresentationTest, ProjectionRequiresRendererOwnershipCertificate) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kRect));
  SelectTool tool;
  tool.onMouseDown(app, Vector2d(15, 15), MouseModifiers{});
  const auto capture = CapturedPresentation::Capture(app.document().document(), Identity(app, 1),
                                                     app.selectedElements());
  const auto resources = FramePresentationTestAccess::resources(
      capture, {RectTile(capture->selection().front(), 10, 0)});
  tool.onMouseMove(app, Vector2d(35, 15), true);
  const auto frame = FramePresentation::Build(resources, Input(app, tool, 1)).frame;
  ASSERT_NE(frame, nullptr);
  EXPECT_FALSE(frame->followsPointer());
  ExpectRectAt(*frame, 10);
}

TEST(FramePresentationTest, SolidPenReplacementCarriesMatchingPaintAndChrome) {
  EditorApp app;
  ASSERT_TRUE(
      app.loadFromString(R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
    <path id="path" d="M10 10 L30 10 L30 30 Z" fill="red"/></svg>)"));
  auto path = *app.document().document().querySelector("#path");
  app.setSelection(path);
  SelectTool tool;
  const Entity entity = path.unsafeEntityHandle().entity();
  const std::array independent{entity};
  const auto capture = CapturedPresentation::Capture(app.document().document(), Identity(app, 1),
                                                     app.selectedElements(), {}, independent,
                                                     app.selectedElements());
  const auto resources = FramePresentationTestAccess::resources(capture, {RectTile(entity, 10, 0)});
  path.setAttribute("d", "M10 10 L60 10 L60 30 Z");
  const auto replacement = CapturedPresentation::Capture(app.document().document(),
                                                         Identity(app, 2), app.selectedElements(),
                                                         {}, independent, {}, std::nullopt, path);
  auto input = Input(app, tool, 1);
  input.detail = SelectionChromeDetail::PathOutlinesOnly;
  input.livePathReplacement = replacement;
  input.decorations.sourceHover = {entity};
  const auto frame = FramePresentation::Build(resources, input).frame;
  ASSERT_NE(frame, nullptr);
  ASSERT_TRUE(frame->replacementPaint().has_value());
  ASSERT_TRUE(frame->replacementPaint()->livePathPreview.has_value());
  ASSERT_THAT(frame->chrome().paths, SizeIs(1));
  EXPECT_EQ(frame->replacementPaint()->livePathPreview->pathDoc,
            frame->chrome().paths.front().pathDoc);
  EXPECT_EQ(frame->chrome().paths.front().pathDoc.bounds(), Box2d::FromXYWH(10, 10, 50, 20));
  EXPECT_FALSE(frame->chrome().livePathPreview.has_value());
  ASSERT_THAT(frame->chrome().hoverPaths, SizeIs(1));
  EXPECT_EQ(frame->chrome().hoverPaths.front().pathDoc,
            frame->replacementPaint()->livePathPreview->pathDoc);
  input.includeChrome = false;
  input.frameId++;
  const auto artworkOnly = FramePresentation::Build(resources, input).frame;
  ASSERT_NE(artworkOnly, nullptr);
  EXPECT_TRUE(artworkOnly->replacementPaint().has_value());
  EXPECT_FALSE(artworkOnly->chromeEnabled());
  EXPECT_THAT(artworkOnly->selectionBounds(), SizeIs(1));
}

TEST(FramePresentationTest, FrameCameraAndGeometryIgnoreLaterUiChanges) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kRect));
  SelectTool tool;
  tool.onMouseDown(app, Vector2d(15, 15), MouseModifiers{});
  const auto capture = Capture(app, 1);
  const auto resources = FramePresentationTestAccess::resources(
      capture, {RectTile(capture->selection().front(), 10, 0)});
  auto input = Input(app, tool, 1);
  const auto frame = FramePresentation::Build(resources, input).frame;
  ASSERT_NE(frame, nullptr);
  input.viewport.zoom = 7;
  input.viewport.panScreenPoint = Vector2d(400, 300);
  tool.onMouseMove(app, Vector2d(50, 60), true);
  ASSERT_TRUE(app.flushFrame());
  ExpectRectAt(*frame, 10);
  EXPECT_EQ(frame->framebufferFromDocument(Vector2d(2, 2)).transformPosition(Vector2d(10, 10)),
            Vector2d(38.6, 38.6));
}

TEST(FramePresentationTest, NestedEffectsAndUnknownPaintRequireRasterization) {
  for (const std::string_view content :
       {R"svg(<g filter="url(#blur)"><rect x="10" y="10" width="20" height="20"/></g>)svg",
        R"(<image x="10" y="10" width="20" height="20" href="missing.png"/>)",
        R"(<use href="#shape"/>)"}) {
    EditorApp app;
    const std::string source =
        std::string(R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
        <defs><filter id="blur"><feGaussianBlur stdDeviation="5"/></filter>
        <rect id="shape" x="10" y="10" width="20" height="20"/></defs><g id="selected">)") +
        std::string(content) + "</g></svg>";
    ASSERT_TRUE(app.loadFromString(source));
    app.setSelection(*app.document().document().querySelector("#selected"));
    const auto captured = Capture(app, 1);
    ASSERT_NE(captured, nullptr);
    EXPECT_FALSE(captured->canProject(captured->selection().front())) << content;
  }
}

TEST(FramePresentationTest, NonzeroViewBoxAndTransformedParentUseOneDocumentSpace) {
  EditorApp app;
  ASSERT_TRUE(
      app.loadFromString(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"
      viewBox="100 200 100 100"><g transform="translate(5 7)">
      <rect id="rect" x="110" y="210" width="20" height="20"/></g></svg>)svg"));
  app.setSelection(*app.document().document().querySelector("#rect"));
  SelectTool tool;
  const auto captured = Capture(app, 1);
  ASSERT_EQ(captured->objects().front().chrome.paths.front().pathDoc.bounds(),
            Box2d::FromXYWH(115, 217, 20, 20));
  auto tile = RectTile(captured->selection().front(), 15, 0);
  tile.canvasOffsetDoc.y = 17;
  const auto resources = FramePresentationTestAccess::resources(captured, {tile});
  auto input = Input(app, tool, 1);
  input.viewport.documentViewBox = Box2d::FromXYWH(100, 200, 100, 100);
  input.viewport.panDocPoint = Vector2d(100, 200);
  input.desired = SelectTool::ActiveDragPreview{.entity = captured->selection().front(),
                                                .contentIdentity = captured->identity(),
                                                .poses = captured->poses(),
                                                .startPoses = captured->poses()};
  input.desired->poses.front().documentFromElement =
      captured->poses().front().documentFromElement * Transform2d::Translate(7, 11);
  const auto frame = FramePresentation::Build(resources, input).frame;
  ASSERT_NE(frame, nullptr);
  EXPECT_TRUE(frame->followsPointer());
  EXPECT_EQ(frame->chrome().paths.front().pathDoc.bounds(), Box2d::FromXYWH(122, 228, 20, 20));
  const auto& placed = frame->tiles().front();
  const auto quad = ComputePresentedTileQuad(
      PresentedFrameTileGeometry{.canvasOffsetDoc = placed.canvasOffsetDoc,
                                 .bitmapDimsDoc = placed.bitmapDimsDoc,
                                 .documentFromCachedDocument = placed.documentFromCachedDocument},
      frame->framebufferFromDocument(Vector2d(1, 1)), std::nullopt);
  ASSERT_TRUE(quad.has_value());
  EXPECT_NEAR(quad->topLeft.x, 22 * 1.93, 1e-8);
  EXPECT_NEAR(quad->topLeft.y, 28 * 1.93, 1e-8);
}

TEST(FramePresentationTest, MultipleObjectsKeepDistinctCapturedPoses) {
  EditorApp app;
  ASSERT_TRUE(
      app.loadFromString(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
    <g transform="translate(5 0)"><rect id="a" x="10" y="10" width="20" height="20"/></g>
    <g transform="translate(20 0)"><rect id="b" x="30" y="10" width="20" height="20"/></g></svg>)svg"));
  app.setSelection(std::vector<svg::SVGElement>{*app.document().document().querySelector("#a"),
                                                *app.document().document().querySelector("#b")});
  SelectTool tool;
  const auto capture = Capture(app, 1);
  ASSERT_THAT(capture->poses(), SizeIs(2));
  const auto resources = FramePresentationTestAccess::resources(
      capture,
      {RectTile(capture->selection()[0], 15, 0), RectTile(capture->selection()[1], 50, 0)});
  auto input = Input(app, tool, 1);
  input.desired = SelectTool::ActiveDragPreview{.entity = capture->selection().front(),
                                                .contentIdentity = capture->identity(),
                                                .poses = capture->poses(),
                                                .startPoses = capture->poses()};
  for (auto& pose : input.desired->poses) {
    pose.documentFromElement = pose.documentFromElement * Transform2d::Translate(10, 5);
  }
  const auto frame = FramePresentation::Build(resources, input).frame;
  ASSERT_NE(frame, nullptr);
  EXPECT_TRUE(frame->followsPointer());
  ASSERT_THAT(frame->chrome().paths, SizeIs(2));
  EXPECT_EQ(frame->chrome().paths[0].pathDoc.bounds(), Box2d::FromXYWH(25, 15, 20, 20));
  EXPECT_EQ(frame->chrome().paths[1].pathDoc.bounds(), Box2d::FromXYWH(60, 15, 20, 20));
  EXPECT_EQ(frame->poses()[0].documentFromElement.translation(), Vector2d(15, 5));
  EXPECT_EQ(frame->poses()[1].documentFromElement.translation(), Vector2d(30, 5));
}

TEST(FramePresentationTest, HeldScaleCanCrossZeroUsingItsNonsingularCapture) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kRect));
  app.setSelection(*app.document().document().querySelector("#rect"));
  SelectTool tool;
  const auto capture = Capture(app, 1);
  const auto resources = FramePresentationTestAccess::resources(
      capture, {RectTile(capture->selection().front(), 10, 0)});
  auto input = Input(app, tool, 1);
  input.desired = SelectTool::ActiveDragPreview{.entity = capture->selection().front(),
                                                .contentIdentity = capture->identity(),
                                                .poses = capture->poses(),
                                                .startPoses = capture->poses()};
  input.desired->poses.front().documentFromElement =
      Transform2d::Translate(-20, -20) * Transform2d::Scale(0.0) * Transform2d::Translate(20, 20);
  const auto collapsed = FramePresentation::Build(resources, input).frame;
  ASSERT_NE(collapsed, nullptr);
  EXPECT_TRUE(collapsed->followsPointer());
  input.frameId++;
  input.desired->poses.front().documentFromElement = Transform2d::Translate(-20, -20) *
                                                     Transform2d::Scale(Vector2d(-1, 1)) *
                                                     Transform2d::Translate(20, 20);
  const auto mirrored = FramePresentation::Build(resources, input, nullptr, collapsed).frame;
  ASSERT_NE(mirrored, nullptr);
  EXPECT_TRUE(mirrored->followsPointer());
  EXPECT_EQ(mirrored->chrome().paths.front().pathDoc.bounds(), Box2d::FromXYWH(10, 10, 20, 20));
  EXPECT_TRUE(FinitePresentationTransform(mirrored->tiles().front().documentFromCachedDocument));
}

TEST(FramePresentationTest, TextGeometryRequiresMatchingDocumentFontAndLayoutIdentity) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kRect));
  app.setSelection(*app.document().document().querySelector("#rect"));
  const auto scene = Capture(app, 1);
  const CapturedPresentation::TextEditing measured{
      .identity = scene->identity(),
      .canvasSize = scene->canvasSize(),
      .subject = scene->selection().front(),
      .caret = SelectionChromeSnapshot::TextCaret{Vector2d(10, 10), Vector2d(10, 30)}};
  const auto withText = [&](const CapturedPresentation::TextEditing& text) {
    return CapturedPresentation::Capture(app.document().document(), scene->identity(),
                                         app.selectedElements(), {}, {}, {}, std::nullopt,
                                         std::nullopt, text);
  };
  ASSERT_TRUE(withText(measured)->textEditing().has_value());
  auto otherDocument = measured;
  ++otherDocument.identity.documentGeneration;
  EXPECT_FALSE(withText(otherDocument)->textEditing().has_value());
  auto otherFont = measured;
  ++otherFont.identity.fontResourceRevision;
  EXPECT_FALSE(withText(otherFont)->textEditing().has_value());
  auto otherLayout = measured;
  ++otherLayout.canvasSize.x;
  EXPECT_FALSE(withText(otherLayout)->textEditing().has_value());
}

}  // namespace
TEST(FramePresentationTest, CurrentCommittedCaptureReplacesAnOlderDisplayedOverride) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kRect));
  SelectTool tool;
  tool.onMouseDown(app, Vector2d(15, 15), MouseModifiers{});
  const auto original = Capture(app, 1);
  const Entity entity = original->selection().front();
  const auto resources =
      FramePresentationTestAccess::resources(original, {RectTile(entity, 10, 0)});
  tool.onMouseMove(app, Vector2d(35, 15), true);
  const auto held = FramePresentation::Build(resources, Input(app, tool, 1)).frame;
  ASSERT_NE(held, nullptr);
  ASSERT_TRUE(app.flushFrame());
  tool.onMouseUp(app, Vector2d(35, 15));
  auto element = app.selectedElements().front().cast<svg::SVGGraphicsElement>();
  element.setTransform(Transform2d::Translate(50, 0));
  const auto current = Capture(app, 2);
  const auto replacement =
      FramePresentationTestAccess::resources(current, {RectTile(entity, 60, 0)});
  auto input = Input(app, tool, 2);
  input.documentIdentity = current->identity();
  input.desired.reset();
  const auto adopted = FramePresentation::Build(replacement, input, nullptr, held).frame;
  ASSERT_NE(adopted, nullptr);
  ExpectRectAt(*adopted, 60);
  EXPECT_THAT(adopted->overrides(), testing::IsEmpty());
}

TEST(FramePresentationTest, FractionalCoverageKeepsCompleteOwnedFamilyUntilPairedReplacement) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kRect));
  SelectTool tool;
  tool.onMouseDown(app, Vector2d(15, 15), MouseModifiers{});
  const auto overview = Capture(app, 1);
  const Entity entity = overview->selection().front();
  tool.onMouseMove(app, Vector2d(35, 15), true);
  ASSERT_TRUE(app.flushFrame());
  const auto active = Capture(app, 2);
  const auto resources = FramePresentationTestAccess::resources(
      active, {RectTile(entity, 10, 20)},
      {.activeTilesViewportBounded = true,
       .overviewInfillAvailable = true,
       .activeRasterDocumentRect = Box2d::FromXYWH(0, 0, 100, 99.98)},
      overview, {RectTile(entity, 10, 0)});
  auto flatTile = RectTile(entt::null, 10, 0);
  flatTile.kind = RenderResult::CompositedTile::Kind::Segment;
  const auto flat = FramePresentationTestAccess::resources(
      active, resources->tiles(), resources->coverage(), overview, {flatTile});
  EXPECT_EQ(FramePresentation::Build(flat, Input(app, tool, 1)).failure,
            FramePresentationFailure::MissingOverview);
  EXPECT_EQ(FramePresentation::Build(resources, Input(app, tool, 1)).failure,
            FramePresentationFailure::MissingOverview);
  const auto complete = FramePresentationTestAccess::resources(overview, {RectTile(entity, 10, 0)});
  auto first = FramePresentation::Build(complete, Input(app, tool, 1)).frame;
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->identity().captureId, overview->identity().captureId);
  EXPECT_TRUE(first->followsPointer());
  ExpectRectAt(*first, 30);
  tool.onMouseMove(app, Vector2d(45, 15), true);
  auto second = FramePresentation::Build(complete, Input(app, tool, 2), nullptr, first).frame;
  ASSERT_NE(second, nullptr);
  EXPECT_TRUE(second->followsPointer());
  ExpectRectAt(*second, 40);
  auto idle = Input(app, tool, 3);
  idle.desired.reset();
  EXPECT_EQ(FramePresentation::Build(resources, idle).failure,
            FramePresentationFailure::MissingOverview);
  app.document().document().setCanvasSize(200, 200);
  const auto changedLayout = Capture(app, 3);
  const auto changed = FramePresentationTestAccess::resources(
      changedLayout, {RectTile(entity, 10, 20)}, resources->coverage(), overview,
      resources->overviewTiles());
  EXPECT_EQ(FramePresentation::Build(changed, Input(app, tool, 4)).failure,
            FramePresentationFailure::MissingOverview);
}

TEST(FramePresentationTest, OlderWholeFamilyCannotRewindNewerCommittedFrame) {
  EditorApp app;
  ASSERT_TRUE(
      app.loadFromString(R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
    <rect id="rect" x="10" y="10" width="20" height="20"/>
    <rect id="peer" x="60" y="10" width="20" height="20"/>
  </svg>)"));
  app.setSelection(*app.document().document().querySelector("#rect"));
  const auto old = Capture(app, 1);
  auto peer = app.document().document().querySelector("#peer")->cast<svg::SVGGraphicsElement>();
  peer.setTransform(Transform2d::Translate(0, 20));
  const auto current = Capture(app, 2);
  ASSERT_TRUE(old->identity().sameContent(current->identity()));
  ASSERT_LT(old->identity().documentRevision, current->identity().documentRevision);
  SelectTool tool;
  auto input = Input(app, tool, 1);
  input.documentIdentity = current->identity();
  const auto frame =
      FramePresentation::Build(FramePresentationTestAccess::resources(
                                   current, {RectTile(current->selection().front(), 10, 0)}),
                               input)
          .frame;
  ASSERT_NE(frame, nullptr);
  EXPECT_TRUE(frame->overrides().empty());
  EXPECT_FALSE(FramePresentation::CanAdopt(*old, {}, frame.get(), false))
      << "An older complete family can rewind a committed unselected object even without "
         "overrides.";
}

}  // namespace donner::editor
