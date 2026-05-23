#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "donner/base/RcString.h"
#include "donner/base/Transform.h"
#include "donner/editor/AsyncRenderer.h"
#include "donner/editor/AttributeWriteback.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/EditorCommand.h"
#include "donner/editor/SelectTool.h"
#include "donner/editor/SelectionAabb.h"
#include "donner/editor/TextPatch.h"
#include "donner/editor/ViewportState.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/renderer/Renderer.h"
#include "gtest/gtest.h"

namespace donner::editor {
namespace {

constexpr std::string_view kLayerStressSvg = R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="264" height="132" viewBox="-24 -12 264 132">
  <defs>
    <filter id="soft-glow" x="-30%" y="-30%" width="160%" height="160%">
      <feGaussianBlur in="SourceGraphic" stdDeviation="3"/>
    </filter>
    <clipPath id="right-clip"><rect x="178" y="12" width="50" height="82" rx="4"/></clipPath>
  </defs>
  <rect id="bg" width="240" height="120" fill="#101622"/>
  <rect id="drag-a" x="18" y="28" width="42" height="34" fill="#ff4f5e"/>
  <g id="glow" filter="url(#soft-glow)">
    <circle cx="112" cy="44" r="22" fill="#ffe45c"/>
    <rect x="96" y="58" width="34" height="18" fill="#ffaf45"/>
  </g>
  <rect id="middle" x="78" y="76" width="56" height="20" fill="#2ee6a6" opacity="0.78"/>
  <g id="right" clip-path="url(#right-clip)" opacity="0.86">
    <rect x="178" y="20" width="50" height="64" fill="#4aa7ff"/>
    <circle id="drag-b" cx="202" cy="52" r="17" fill="#e6d6ff"/>
  </g>
</svg>
)svg";

bool ContainsBoxWithTolerance(const Box2d& outer, const Box2d& inner, double tolerance) {
  const Box2d inflated = outer.inflatedBy(tolerance);
  return inflated.contains(inner.topLeft) && inflated.contains(inner.bottomRight);
}

Box2d TileScreenRect(const RenderResult::CompositedTile& tile, const ViewportState& viewport) {
  const Box2d imageScreenRect = viewport.imageScreenRect();
  const double pxPerDoc = viewport.pixelsPerDocUnit();
  const Vector2d originDoc = tile.canvasOffsetDoc + tile.dragTranslationDoc;
  const Vector2d topLeft =
      imageScreenRect.topLeft + Vector2d(originDoc.x * pxPerDoc, originDoc.y * pxPerDoc);
  const Vector2d size = tile.bitmapDimsDoc * pxPerDoc;
  return Box2d(topLeft, topLeft + size);
}

std::optional<TextPatch> BuildTransformPatch(std::string_view source,
                                             const AttributeWritebackTarget& target,
                                             const Transform2d& elementFromParent) {
  const RcString serialized = toSVGTransformString(elementFromParent);
  if (std::string_view(serialized).empty()) {
    return buildAttributeRemoveWriteback(source, target, "transform");
  }

  return buildAttributeWriteback(source, target, "transform", std::string_view(serialized));
}

bool DrainDragWritebackAndQueueReparse(EditorApp& app, SelectTool& selectTool,
                                       std::string* source) {
  auto completed = selectTool.consumeCompletedDragWriteback();
  if (!completed.has_value()) {
    return false;
  }

  std::vector<TextPatch> patches;
  patches.reserve(1u + completed->extras.size());
  if (auto patch = BuildTransformPatch(*source, completed->target, completed->transform);
      patch.has_value()) {
    patches.push_back(*std::move(patch));
  }
  for (const auto& extra : completed->extras) {
    if (auto patch = BuildTransformPatch(*source, extra.target, extra.transform);
        patch.has_value()) {
      patches.push_back(*std::move(patch));
    }
  }
  if (patches.empty()) {
    return false;
  }

  const auto result = applyPatches(*source, patches);
  if (result.applied != patches.size()) {
    return false;
  }

  app.applyMutation(EditorCommand::ReplaceDocumentCommand(*source, /*preserveUndoOnReparse=*/true));
  return true;
}

bool DrainElementRemoveWritebacksAndQueueReparse(EditorApp& app, std::string* source) {
  auto completed = app.consumeElementRemoveWritebacks();
  if (completed.empty()) {
    return false;
  }

  std::vector<TextPatch> patches;
  patches.reserve(completed.size());
  for (const auto& writeback : completed) {
    if (auto patch = buildElementRemoveWriteback(*source, writeback.target); patch.has_value()) {
      patches.push_back(*std::move(patch));
    }
  }
  if (patches.empty()) {
    return false;
  }

  const auto result = applyPatches(*source, patches);
  if (result.applied != patches.size()) {
    return false;
  }

  app.applyMutation(EditorCommand::ReplaceDocumentCommand(*source, /*preserveUndoOnReparse=*/true));
  return true;
}

std::optional<RenderResult> RequestRenderAndWait(AsyncRenderer& asyncRenderer,
                                                 svg::Renderer& renderer, EditorApp& app,
                                                 SelectTool& selectTool, std::uint64_t version,
                                                 std::string_view phase,
                                                 bool repostWhileBusy = false) {
  const auto buildRequest = [&] {
    RenderRequest request(renderer, app.document().document());
    request.version = version;
    request.documentGeneration = app.document().documentGeneration();
    request.structuralRemap = app.document().consumePendingStructuralRemap();
    if (app.selectedElement().has_value() &&
        app.selectedElement()->isa<svg::SVGGraphicsElement>()) {
      request.selectedEntity = app.selectedElement()->entityHandle().entity();
    }
    if (auto preview = selectTool.activeDragPreview(); preview.has_value()) {
      request.dragPreview = RenderRequest::DragPreview{
          .entity = preview->entity,
          .interactionKind = svg::compositor::InteractionHint::ActiveDrag,
          .translation = preview->translation,
          .documentFromCachedDocument = preview->documentFromCachedDocument,
          .dragGeneration = preview->dragGeneration,
      };
    }
    return request;
  };

  asyncRenderer.requestRender(buildRequest());
  if (repostWhileBusy) {
    asyncRenderer.requestRender(buildRequest());
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    auto result = asyncRenderer.pollResult();
    if (result.has_value()) {
      return result;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  ADD_FAILURE() << "render timed out during " << phase;
  return std::nullopt;
}

svg::RendererBitmap RenderReference(std::string_view source, const Vector2i& canvasSize) {
  EditorApp referenceApp;
  EXPECT_TRUE(referenceApp.loadFromString(source));
  referenceApp.document().document().setCanvasSize(canvasSize.x, canvasSize.y);

  svg::Renderer referenceRenderer;
  referenceRenderer.draw(referenceApp.document().document());
  return referenceRenderer.takeSnapshot();
}

void ExpectCompositedTileGeometryCoherent(const RenderResult& result, const Vector2i& canvasSize,
                                          const Box2d& viewBox, std::string_view phase) {
  ASSERT_TRUE(result.compositedPreview.has_value()) << phase << ": missing composited preview";
  ASSERT_FALSE(result.compositedPreview->tiles.empty()) << phase << ": no composited tiles";

  const double viewBoxWidth = viewBox.width();
  const double viewBoxHeight = viewBox.height();
  ASSERT_GT(viewBoxWidth, 0.0) << phase << ": invalid viewBox width";
  ASSERT_GT(viewBoxHeight, 0.0) << phase << ": invalid viewBox height";

  const double pixelsPerDocX = static_cast<double>(canvasSize.x) / viewBoxWidth;
  const double pixelsPerDocY = static_cast<double>(canvasSize.y) / viewBoxHeight;

  int tileCount = 0;
  int nonEmptyTiles = 0;
  for (const RenderResult::CompositedTile& tile : result.compositedPreview->tiles) {
    ++tileCount;
    const Vector2d displayOffsetDoc = tile.canvasOffsetDoc + tile.dragTranslationDoc;
    EXPECT_TRUE(std::isfinite(displayOffsetDoc.x)) << phase << ": non-finite tile x offset";
    EXPECT_TRUE(std::isfinite(displayOffsetDoc.y)) << phase << ": non-finite tile y offset";
    EXPECT_TRUE(std::isfinite(tile.bitmapDimsDoc.x)) << phase << ": non-finite tile width";
    EXPECT_TRUE(std::isfinite(tile.bitmapDimsDoc.y)) << phase << ": non-finite tile height";

    // Allow filter blur and clip padding to extend modestly outside the
    // viewBox, but catch the bad class where zoom applies twice and sends a
    // tile far outside the canvas or gives it a wildly scaled doc extent.
    EXPECT_GT(displayOffsetDoc.x + tile.bitmapDimsDoc.x, -80.0) << phase << ": tile off left";
    EXPECT_GT(displayOffsetDoc.y + tile.bitmapDimsDoc.y, -80.0) << phase << ": tile off top";
    EXPECT_LT(displayOffsetDoc.x, viewBoxWidth + 80.0) << phase << ": tile off right";
    EXPECT_LT(displayOffsetDoc.y, viewBoxHeight + 80.0) << phase << ": tile off bottom";

    if (tile.bitmap.empty()) {
      continue;
    }
    ++nonEmptyTiles;

    EXPECT_NEAR(tile.bitmapDimsDoc.x * pixelsPerDocX, static_cast<double>(tile.bitmap.dimensions.x),
                2.0)
        << phase << ": tile " << tile.id << " width metadata no longer matches bitmap pixels";
    EXPECT_NEAR(tile.bitmapDimsDoc.y * pixelsPerDocY, static_cast<double>(tile.bitmap.dimensions.y),
                2.0)
        << phase << ": tile " << tile.id << " height metadata no longer matches bitmap pixels";
  }

  EXPECT_GE(tileCount, 3) << phase << ": stress scene did not produce enough layer tiles";
  EXPECT_GE(nonEmptyTiles, 1) << phase << ": stress scene did not publish any tile pixels";
}

class EditorLayerStressTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(app_.loadFromString(kLayerStressSvg));
    source_ = std::string(kLayerStressSvg);
    viewport_.paneOrigin = Vector2d(560.0, 20.0);
    viewport_.paneSize = Vector2d(900.0, 540.0);
    viewport_.devicePixelRatio = 1.0;
    viewport_.documentViewBox = CurrentViewBox();
    viewport_.resetTo100Percent();
    SetCanvasSize(viewport_.desiredCanvasSize());
  }

  void SetCanvasSize(const Vector2i& canvasSize) {
    canvasSize_ = canvasSize;
    app_.document().document().setCanvasSize(canvasSize.x, canvasSize.y);
  }

  [[nodiscard]] Box2d CurrentViewBox() const {
    if (auto viewBox = app_.document().document().svgElement().viewBox()) {
      return *viewBox;
    }
    return Box2d::FromXYWH(0.0, 0.0, static_cast<double>(canvasSize_.x),
                           static_cast<double>(canvasSize_.y));
  }

  void CommitViewportCanvasSize() { SetCanvasSize(viewport_.desiredCanvasSize()); }

  RenderResult RenderPhase(std::string_view phase, bool repostWhileBusy = false) {
    auto result = RequestRenderAndWait(asyncRenderer_, renderer_, app_, selectTool_, ++version_,
                                       phase, repostWhileBusy);
    EXPECT_FALSE(asyncRenderer_.isBusy()) << phase << ": worker stayed busy after result";
    EXPECT_TRUE(result.has_value()) << phase;
    return std::move(*result);
  }

  RenderRequest BuildRenderRequest(std::uint64_t version) {
    RenderRequest request(renderer_, app_.document().document());
    request.version = version;
    request.documentGeneration = app_.document().documentGeneration();
    request.structuralRemap = app_.document().consumePendingStructuralRemap();
    if (app_.selectedElement().has_value() &&
        app_.selectedElement()->isa<svg::SVGGraphicsElement>()) {
      request.selectedEntity = app_.selectedElement()->entityHandle().entity();
    }
    if (auto preview = selectTool_.activeDragPreview(); preview.has_value()) {
      request.dragPreview = RenderRequest::DragPreview{
          .entity = preview->entity,
          .interactionKind = svg::compositor::InteractionHint::ActiveDrag,
          .translation = preview->translation,
          .documentFromCachedDocument = preview->documentFromCachedDocument,
          .dragGeneration = preview->dragGeneration,
      };
    }
    return request;
  }

  void WaitForRendererIdle(std::string_view phase) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (asyncRenderer_.isBusy() && std::chrono::steady_clock::now() < deadline) {
      (void)asyncRenderer_.pollResult();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_FALSE(asyncRenderer_.isBusy()) << phase << ": renderer stayed busy after cancel";
  }

  void PostSupersededRenderAndCancel(std::string_view phase) {
    asyncRenderer_.requestRender(BuildRenderRequest(++version_));
    asyncRenderer_.requestRender(BuildRenderRequest(version_));
    ASSERT_TRUE(asyncRenderer_.isBusy()) << phase << ": render did not enter busy state";
    asyncRenderer_.cancelInFlight();
    WaitForRendererIdle(phase);
    EXPECT_FALSE(asyncRenderer_.pollResult().has_value())
        << phase << ": cancelled render published a stale result";
  }

  void ClickSelectAfterCancellingBusyRender(std::string_view phase, const Vector2d& documentPoint,
                                            std::string_view expectedId) {
    ASSERT_TRUE(asyncRenderer_.isBusy()) << phase << ": expected a busy render before click";
    asyncRenderer_.cancelInFlight();
    WaitForRendererIdle(phase);
    EXPECT_FALSE(asyncRenderer_.pollResult().has_value())
        << phase << ": cancelled render published a stale result";

    selectTool_.onMouseDown(app_, documentPoint, MouseModifiers{});
    ASSERT_TRUE(app_.selectedElement().has_value()) << phase << ": click did not select anything";
    EXPECT_EQ(app_.selectedElement()->id(), expectedId) << phase << ": wrong element selected";
    selectTool_.onMouseUp(app_, documentPoint);
    EXPECT_FALSE(selectTool_.isDragging()) << phase << ": click left a drag latched";

    RenderResult result = RenderPhase(std::string(phase) + " post-click render",
                                      /*repostWhileBusy=*/true);
    EXPECT_FALSE(asyncRenderer_.isBusy()) << phase << ": worker stayed busy after click render";
  }

  void ClickSelectThroughBusyGate(std::string_view phase, const Vector2d& documentPoint,
                                  std::string_view expectedId) {
    PostSupersededRenderAndCancel(phase);
    ASSERT_FALSE(asyncRenderer_.isBusy()) << phase << ": synthetic cancel did not idle";
    asyncRenderer_.requestRender(BuildRenderRequest(++version_));
    ClickSelectAfterCancellingBusyRender(phase, documentPoint, expectedId);
  }

  void ReleaseWritebackAndPostSettlingRender(std::string_view phase,
                                             const Vector2d& documentPoint) {
    selectTool_.onMouseUp(app_, documentPoint);
    ASSERT_TRUE(DrainDragWritebackAndQueueReparse(app_, selectTool_, &source_))
        << phase << ": missing drag writeback";
    ASSERT_TRUE(app_.flushFrame()) << phase << ": writeback reparse did not flush";
    SetCanvasSize(canvasSize_);
    asyncRenderer_.requestRender(BuildRenderRequest(++version_));
    ASSERT_TRUE(asyncRenderer_.isBusy()) << phase << ": settling render did not start";
  }

  void DeleteSelectedAndPostSettlingRender(std::string_view phase) {
    const std::vector<svg::SVGElement> selected = app_.selectedElements();
    ASSERT_FALSE(selected.empty()) << phase << ": nothing selected to delete";

    app_.setSelection(std::nullopt);
    for (const svg::SVGElement& element : selected) {
      if (auto target = captureAttributeWritebackTarget(element); target.has_value()) {
        app_.enqueueElementRemoveWriteback(EditorApp::CompletedElementRemoveWriteback{
            .target = *target,
        });
      }
      app_.applyMutation(EditorCommand::DeleteElementCommand(element));
    }
    ASSERT_TRUE(app_.flushFrame()) << phase << ": delete mutation did not flush";

    ASSERT_TRUE(DrainElementRemoveWritebacksAndQueueReparse(app_, &source_))
        << phase << ": delete source writeback failed";
    ASSERT_TRUE(app_.flushFrame()) << phase << ": delete writeback reparse did not flush";
    SetCanvasSize(canvasSize_);

    asyncRenderer_.requestRender(BuildRenderRequest(++version_));
    ASSERT_TRUE(asyncRenderer_.isBusy()) << phase << ": delete settling render did not start";
  }

  void DeleteSelectedAfterCancellingBusyRender(std::string_view phase) {
    ASSERT_TRUE(asyncRenderer_.isBusy()) << phase << ": expected a busy render before delete";
    asyncRenderer_.cancelInFlight();
    WaitForRendererIdle(phase);
    EXPECT_FALSE(asyncRenderer_.pollResult().has_value())
        << phase << ": cancelled render published a stale result";

    DeleteSelectedAndPostSettlingRender(phase);
    RenderResult result = RenderPhase(std::string(phase) + " post-delete render",
                                      /*repostWhileBusy=*/true);
    EXPECT_FALSE(asyncRenderer_.isBusy()) << phase << ": worker stayed busy after delete render";
    svg::RendererBitmap reference = RenderReference(source_, canvasSize_);
    tests::CompareBitmapToBitmap(result.bitmap, reference, phase);
  }

  void BeginDragAtScreenPoint(std::string_view phase, const Vector2d& screenPoint,
                              std::string_view expectedId) {
    selectTool_.onMouseDown(app_, viewport_.screenToDocument(screenPoint), MouseModifiers{});
    ASSERT_TRUE(app_.selectedElement().has_value()) << phase << ": drag start selected nothing";
    EXPECT_EQ(app_.selectedElement()->id(), expectedId) << phase << ": wrong drag target selected";
    ASSERT_TRUE(selectTool_.isDragging()) << phase << ": drag did not start";

    RenderResult result = RenderPhase(std::string(phase) + " prewarm", /*repostWhileBusy=*/true);
    EXPECT_FALSE(asyncRenderer_.isBusy()) << phase << ": worker stayed busy after drag prewarm";
  }

  void ZoomAroundScreenPoint(std::string_view phase, const Vector2d& screenPoint,
                             double zoomFactor) {
    const Vector2d docUnderCursorBeforeZoom = viewport_.screenToDocument(screenPoint);
    viewport_.zoomAround(viewport_.zoom * zoomFactor, screenPoint);
    EXPECT_NEAR(viewport_.documentToScreen(docUnderCursorBeforeZoom).x, screenPoint.x, 1e-6)
        << phase << ": zoom focal point drifted in x";
    EXPECT_NEAR(viewport_.documentToScreen(docUnderCursorBeforeZoom).y, screenPoint.y, 1e-6)
        << phase << ": zoom focal point drifted in y";
  }

  void ExpectSettledMatchesReference(std::string_view phase) {
    RenderResult result = RenderPhase(phase);
    EXPECT_LT(result.workerMs, 1500.0) << phase << ": small stress scene rendered too slowly";
    svg::RendererBitmap reference = RenderReference(source_, canvasSize_);
    tests::CompareBitmapToBitmap(result.bitmap, reference, phase);
  }

  void DragTo(std::string_view phase, const Vector2d& documentPoint,
              const std::optional<Vector2i>& newCanvasSize = std::nullopt,
              bool repostWhileBusy = false) {
    selectTool_.onMouseMove(app_, documentPoint, /*buttonHeld=*/true);
    ASSERT_TRUE(app_.flushFrame()) << phase << ": drag move produced no DOM mutation";
    if (newCanvasSize.has_value()) {
      SetCanvasSize(*newCanvasSize);
    }

    RenderResult result = RenderPhase(phase, repostWhileBusy);
    EXPECT_LT(result.workerMs, 1500.0) << phase << ": active drag render exceeded budget";
    ExpectCompositedTileGeometryCoherent(result, canvasSize_, CurrentViewBox(), phase);
    ExpectDragTileContainsSelection(phase, result);
  }

  void DragScreenTo(std::string_view phase, const Vector2d& screenPoint, bool commitCanvas,
                    bool repostWhileBusy) {
    selectTool_.onMouseMove(app_, viewport_.screenToDocument(screenPoint), /*buttonHeld=*/true);
    ASSERT_TRUE(app_.flushFrame()) << phase << ": drag move produced no DOM mutation";
    if (commitCanvas) {
      CommitViewportCanvasSize();
    }

    RenderResult result = RenderPhase(phase, repostWhileBusy);
    EXPECT_LT(result.workerMs, 1500.0) << phase << ": active drag render exceeded budget";
    ExpectCompositedTileGeometryCoherent(result, canvasSize_, CurrentViewBox(), phase);
    ExpectDragTileContainsSelection(phase, result);
  }

  void ExpectDragTileContainsSelection(std::string_view phase, const RenderResult& result) {
    const auto activeDrag = selectTool_.activeDragPreview();
    if (!activeDrag.has_value()) {
      return;
    }
    ASSERT_TRUE(result.compositedPreview.has_value()) << phase << ": missing drag preview";

    const std::vector<Box2d> bounds =
        SnapshotSelectionWorldBounds(std::span<const svg::SVGElement>(app_.selectedElements()));
    ASSERT_FALSE(bounds.empty()) << phase << ": selected element has no bounds";
    Box2d selectedBounds = bounds.front();
    for (std::size_t i = 1; i < bounds.size(); ++i) {
      selectedBounds.addBox(bounds[i]);
    }
    const Box2d selectedScreenBounds = viewport_.documentToScreen(selectedBounds);

    bool sawDragTile = false;
    for (const RenderResult::CompositedTile& tile : result.compositedPreview->tiles) {
      if (!tile.isDragTarget) {
        continue;
      }
      sawDragTile = true;
      const Box2d tileScreenBounds = TileScreenRect(tile, viewport_);
      EXPECT_TRUE(ContainsBoxWithTolerance(tileScreenBounds, selectedScreenBounds, 3.0))
          << phase << ": drag tile no longer covers selected bounds at zoom " << viewport_.zoom
          << "\n  tile screen=" << tileScreenBounds
          << "\n  selected screen=" << selectedScreenBounds;
    }
    EXPECT_TRUE(sawDragTile) << phase << ": no active drag tile in composited preview";
  }

  void ReleaseAndWriteback(std::string_view phase, const Vector2d& documentPoint) {
    selectTool_.onMouseUp(app_, documentPoint);
    ASSERT_TRUE(DrainDragWritebackAndQueueReparse(app_, selectTool_, &source_))
        << phase << ": missing drag writeback";
    ASSERT_TRUE(app_.flushFrame()) << phase << ": writeback reparse did not flush";
    SetCanvasSize(canvasSize_);
    ExpectSettledMatchesReference(phase);
  }

  EditorApp app_;
  SelectTool selectTool_;
  svg::Renderer renderer_;
  AsyncRenderer asyncRenderer_;
  std::string source_;
  ViewportState viewport_;
  Vector2i canvasSize_;
  std::uint64_t version_ = 0;
};

TEST_F(EditorLayerStressTest, DragZoomWritebackStressKeepsCompositionAlignedAndDoesNotHang) {
  ExpectSettledMatchesReference("cold");

  selectTool_.onMouseDown(app_, Vector2d(32.0, 42.0), MouseModifiers{});
  EXPECT_TRUE(app_.hasSelection());
  RenderPhase("drag-a mousedown prewarm");
  DragTo("drag-a move at 1x", Vector2d(45.0, 50.0));
  DragTo("drag-a move after zoom in", Vector2d(54.0, 54.0), Vector2i(360, 180),
         /*repostWhileBusy=*/true);
  DragTo("drag-a move after zoom out", Vector2d(60.0, 56.0), Vector2i(288, 144));
  ReleaseAndWriteback("drag-a release writeback", Vector2d(60.0, 56.0));

  selectTool_.onMouseDown(app_, Vector2d(112.0, 44.0), MouseModifiers{});
  EXPECT_TRUE(app_.hasSelection());
  RenderPhase("filter mousedown prewarm");
  DragTo("filter move at 1.2x", Vector2d(126.0, 52.0));
  DragTo("filter move after zoom in", Vector2d(136.0, 58.0), Vector2i(480, 240),
         /*repostWhileBusy=*/true);
  ReleaseAndWriteback("filter release writeback", Vector2d(136.0, 58.0));

  selectTool_.onMouseDown(app_, Vector2d(202.0, 52.0), MouseModifiers{});
  EXPECT_TRUE(app_.hasSelection());
  RenderPhase("right clipped mousedown prewarm");
  DragTo("right clipped move after zoom out", Vector2d(192.0, 61.0), Vector2i(240, 120),
         /*repostWhileBusy=*/true);
  ReleaseAndWriteback("right clipped release writeback", Vector2d(192.0, 61.0));

  SetCanvasSize(Vector2i(360, 180));
  ExpectSettledMatchesReference("final zoomed settle");

  EXPECT_EQ(asyncRenderer_.compositorResetCountForTesting(), 0u)
      << "structurally-equivalent drag writebacks should remap, not reset the compositor";
  EXPECT_EQ(asyncRenderer_.compositorReconstructCountForTesting(), 1u)
      << "stress sequence rebuilt the compositor; cached layers should survive";
}

TEST_F(EditorLayerStressTest, RepeatedScreenSpaceZoomDragKeepsActiveTileAnchoredAndDoesNotHang) {
  ExpectSettledMatchesReference("cold screen-space stress");

  const Vector2d startDoc(112.0, 44.0);
  Vector2d cursorScreen = viewport_.documentToScreen(startDoc);
  selectTool_.onMouseDown(app_, viewport_.screenToDocument(cursorScreen), MouseModifiers{});
  ASSERT_TRUE(app_.hasSelection());
  RenderPhase("screen-space mousedown prewarm", /*repostWhileBusy=*/true);

  for (int i = 0; i < 18; ++i) {
    cursorScreen += Vector2d(7.0 + static_cast<double>(i % 3), (i % 2 == 0) ? 3.0 : -2.0);

    const double zoomFactor = (i % 4 == 0) ? 1.18 : ((i % 4 == 2) ? 0.87 : 1.04);
    const double newZoom = viewport_.zoom * zoomFactor;
    const Vector2d docUnderCursorBeforeZoom = viewport_.screenToDocument(cursorScreen);
    viewport_.zoomAround(newZoom, cursorScreen);
    EXPECT_NEAR(viewport_.documentToScreen(docUnderCursorBeforeZoom).x, cursorScreen.x, 1e-6)
        << "zoom focal point drifted on iteration " << i;
    EXPECT_NEAR(viewport_.documentToScreen(docUnderCursorBeforeZoom).y, cursorScreen.y, 1e-6)
        << "zoom focal point drifted on iteration " << i;

    if (i % 5 == 3) {
      viewport_.panBy(Vector2d(-4.0, 6.0));
      cursorScreen += Vector2d(-4.0, 6.0);
    }

    const bool commitCanvas = (i % 3) == 0;
    const bool repostWhileBusy = (i % 2) == 0;
    DragScreenTo("screen-space zoom drag iteration " + std::to_string(i), cursorScreen,
                 commitCanvas, repostWhileBusy);
  }

  ReleaseAndWriteback("screen-space zoom drag release", viewport_.screenToDocument(cursorScreen));
  CommitViewportCanvasSize();
  ExpectSettledMatchesReference("screen-space zoom drag settled");

  ClickSelectThroughBusyGate("busy-gated click selects drag-a", Vector2d(32.0, 42.0), "drag-a");
  ClickSelectThroughBusyGate("busy-gated click selects middle", Vector2d(106.0, 86.0), "middle");
  ClickSelectThroughBusyGate("busy-gated click selects clipped group", Vector2d(202.0, 52.0),
                             "right");
  ClickSelectThroughBusyGate("busy-gated click selects drag-a again", Vector2d(32.0, 42.0),
                             "drag-a");

  EXPECT_FALSE(asyncRenderer_.isBusy()) << "worker stayed busy after repeated zoom/drag stress";
}

TEST_F(EditorLayerStressTest, MixedClicksDragsDeletesAndFilteredMovesStayResponsive) {
  ExpectSettledMatchesReference("mixed delete/filter cold");

  ClickSelectThroughBusyGate("mixed delete/filter click filtered glow", Vector2d(112.0, 44.0),
                             "glow");
  Vector2d cursorScreen = viewport_.documentToScreen(Vector2d(112.0, 44.0));
  BeginDragAtScreenPoint("mixed delete/filter filtered glow drag", cursorScreen, "glow");
  cursorScreen += Vector2d(38.0, -20.0);
  ZoomAroundScreenPoint("mixed delete/filter filtered glow zoom-in", cursorScreen, 1.42);
  DragScreenTo("mixed delete/filter filtered glow move after zoom", cursorScreen,
               /*commitCanvas=*/true, /*repostWhileBusy=*/true);
  viewport_.panBy(Vector2d(18.0, 12.0));
  cursorScreen += Vector2d(16.0, 22.0);
  DragScreenTo("mixed delete/filter filtered glow move after pan", cursorScreen,
               /*commitCanvas=*/false, /*repostWhileBusy=*/true);
  ReleaseWritebackAndPostSettlingRender("mixed delete/filter glow release posts settle",
                                        viewport_.screenToDocument(cursorScreen));

  ClickSelectAfterCancellingBusyRender("mixed delete/filter click middle while glow settle busy",
                                       Vector2d(106.0, 86.0), "middle");
  asyncRenderer_.requestRender(BuildRenderRequest(++version_));
  DeleteSelectedAfterCancellingBusyRender("mixed delete/filter delete middle while prewarm busy");

  ClickSelectThroughBusyGate("mixed delete/filter old middle point selects bg",
                             Vector2d(106.0, 86.0), "bg");
  ClickSelectThroughBusyGate("mixed delete/filter click drag-a after delete", Vector2d(32.0, 42.0),
                             "drag-a");
  cursorScreen = viewport_.documentToScreen(Vector2d(32.0, 42.0));
  BeginDragAtScreenPoint("mixed delete/filter drag-a", cursorScreen, "drag-a");
  cursorScreen += Vector2d(-18.0, 20.0);
  ZoomAroundScreenPoint("mixed delete/filter drag-a zoom-out", cursorScreen, 0.78);
  DragScreenTo("mixed delete/filter drag-a move after zoom", cursorScreen, /*commitCanvas=*/true,
               /*repostWhileBusy=*/true);
  ReleaseWritebackAndPostSettlingRender("mixed delete/filter drag-a release posts settle",
                                        viewport_.screenToDocument(cursorScreen));

  ClickSelectAfterCancellingBusyRender("mixed delete/filter click right while drag-a settle busy",
                                       Vector2d(202.0, 52.0), "right");
  cursorScreen = viewport_.documentToScreen(Vector2d(202.0, 52.0));
  BeginDragAtScreenPoint("mixed delete/filter right drag", cursorScreen, "right");
  cursorScreen += Vector2d(-22.0, 26.0);
  ZoomAroundScreenPoint("mixed delete/filter right zoom-in", cursorScreen, 1.2);
  DragScreenTo("mixed delete/filter right move after zoom", cursorScreen, /*commitCanvas=*/false,
               /*repostWhileBusy=*/true);
  ReleaseWritebackAndPostSettlingRender("mixed delete/filter right release posts settle",
                                        viewport_.screenToDocument(cursorScreen));

  ClickSelectAfterCancellingBusyRender("mixed delete/filter click bg while right settle busy",
                                       Vector2d(8.0, 108.0), "bg");
  ExpectSettledMatchesReference("mixed delete/filter final settle");

  EXPECT_FALSE(asyncRenderer_.isBusy())
      << "worker stayed busy after mixed delete/filter interactions";
}

TEST_F(EditorLayerStressTest, MixedClicksDragsAndZoomsKeepSelectionResponsive) {
  ExpectSettledMatchesReference("mixed cold");

  ClickSelectThroughBusyGate("mixed click drag-a", Vector2d(32.0, 42.0), "drag-a");
  Vector2d cursorScreen = viewport_.documentToScreen(Vector2d(32.0, 42.0));
  BeginDragAtScreenPoint("mixed drag-a", cursorScreen, "drag-a");
  cursorScreen += Vector2d(42.0, 18.0);
  ZoomAroundScreenPoint("mixed drag-a zoom-in", cursorScreen, 1.35);
  DragScreenTo("mixed drag-a move after zoom", cursorScreen, /*commitCanvas=*/true,
               /*repostWhileBusy=*/true);
  viewport_.panBy(Vector2d(22.0, -10.0));
  cursorScreen += Vector2d(30.0, -8.0);
  DragScreenTo("mixed drag-a move after pan", cursorScreen, /*commitCanvas=*/false,
               /*repostWhileBusy=*/true);
  ReleaseWritebackAndPostSettlingRender("mixed drag-a release posts settle",
                                        viewport_.screenToDocument(cursorScreen));

  ClickSelectAfterCancellingBusyRender("mixed click middle while drag-a settle busy",
                                       Vector2d(106.0, 86.0), "middle");
  cursorScreen = viewport_.documentToScreen(Vector2d(106.0, 86.0));
  BeginDragAtScreenPoint("mixed middle drag", cursorScreen, "middle");
  cursorScreen += Vector2d(-26.0, 20.0);
  ZoomAroundScreenPoint("mixed middle zoom-out", cursorScreen, 0.82);
  DragScreenTo("mixed middle move after zoom", cursorScreen, /*commitCanvas=*/false,
               /*repostWhileBusy=*/true);
  viewport_.panBy(Vector2d(-18.0, 14.0));
  cursorScreen += Vector2d(36.0, 10.0);
  DragScreenTo("mixed middle move after pan", cursorScreen, /*commitCanvas=*/true,
               /*repostWhileBusy=*/false);
  ReleaseWritebackAndPostSettlingRender("mixed middle release posts settle",
                                        viewport_.screenToDocument(cursorScreen));

  ClickSelectAfterCancellingBusyRender("mixed click right while middle settle busy",
                                       Vector2d(202.0, 52.0), "right");
  cursorScreen = viewport_.documentToScreen(Vector2d(202.0, 52.0));
  BeginDragAtScreenPoint("mixed right drag", cursorScreen, "right");
  cursorScreen += Vector2d(-18.0, 24.0);
  ZoomAroundScreenPoint("mixed right zoom-in", cursorScreen, 1.22);
  DragScreenTo("mixed right move after zoom", cursorScreen, /*commitCanvas=*/true,
               /*repostWhileBusy=*/true);
  ReleaseWritebackAndPostSettlingRender("mixed right release posts settle",
                                        viewport_.screenToDocument(cursorScreen));

  ClickSelectAfterCancellingBusyRender("mixed click bg while right settle busy",
                                       Vector2d(8.0, 108.0), "bg");
  ExpectSettledMatchesReference("mixed final settle");

  EXPECT_FALSE(asyncRenderer_.isBusy()) << "worker stayed busy after mixed interactions";
}

}  // namespace
}  // namespace donner::editor
