#include <chrono>
#include <cmath>
#include <optional>
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
#include "donner/editor/TextPatch.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/renderer/Renderer.h"
#include "gtest/gtest.h"

namespace donner::editor {
namespace {

constexpr std::string_view kLayerStressSvg = R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="240" height="120" viewBox="0 0 240 120">
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

std::optional<RenderResult> RequestRenderAndWait(AsyncRenderer& asyncRenderer,
                                                 svg::Renderer& renderer, EditorApp& app,
                                                 SelectTool& selectTool, std::uint64_t version,
                                                 std::string_view phase,
                                                 bool repostWhileBusy = false) {
  const auto buildRequest = [&] {
    RenderRequest request;
    request.renderer = &renderer;
    request.document = &app.document().document();
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
      };
    }
    return request;
  };

  asyncRenderer.requestRender(buildRequest());
  if (repostWhileBusy) {
    asyncRenderer.requestRender(buildRequest());
  }

  std::optional<RenderResult> lastResult;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    auto result = asyncRenderer.pollResult();
    if (result.has_value()) {
      lastResult = std::move(result);
      if (lastResult->stage == RenderResult::Stage::Final) {
        return lastResult;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  ADD_FAILURE() << "render timed out during " << phase;
  return lastResult;
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
                                          std::string_view phase) {
  ASSERT_TRUE(result.compositedPreview.has_value()) << phase << ": missing composited preview";
  ASSERT_FALSE(result.compositedPreview->tiles.empty()) << phase << ": no composited tiles";

  constexpr double kViewBoxWidth = 240.0;
  constexpr double kViewBoxHeight = 120.0;
  const double pixelsPerDocX = static_cast<double>(canvasSize.x) / kViewBoxWidth;
  const double pixelsPerDocY = static_cast<double>(canvasSize.y) / kViewBoxHeight;

  int nonEmptyTiles = 0;
  for (const RenderResult::CompositedTile& tile : result.compositedPreview->tiles) {
    if (tile.bitmap.empty()) {
      continue;
    }
    ++nonEmptyTiles;

    const Vector2d displayOffsetDoc = tile.canvasOffsetDoc + tile.dragTranslationDoc;
    EXPECT_TRUE(std::isfinite(displayOffsetDoc.x)) << phase << ": non-finite tile x offset";
    EXPECT_TRUE(std::isfinite(displayOffsetDoc.y)) << phase << ": non-finite tile y offset";
    EXPECT_TRUE(std::isfinite(tile.bitmapDimsDoc.x)) << phase << ": non-finite tile width";
    EXPECT_TRUE(std::isfinite(tile.bitmapDimsDoc.y)) << phase << ": non-finite tile height";

    EXPECT_NEAR(tile.bitmapDimsDoc.x * pixelsPerDocX, static_cast<double>(tile.bitmap.dimensions.x),
                2.0)
        << phase << ": tile " << tile.id << " width metadata no longer matches bitmap pixels";
    EXPECT_NEAR(tile.bitmapDimsDoc.y * pixelsPerDocY, static_cast<double>(tile.bitmap.dimensions.y),
                2.0)
        << phase << ": tile " << tile.id << " height metadata no longer matches bitmap pixels";

    // Allow filter blur and clip padding to extend modestly outside the
    // viewBox, but catch the bad class where zoom applies twice and sends a
    // tile far outside the canvas or gives it a wildly scaled doc extent.
    EXPECT_GT(displayOffsetDoc.x + tile.bitmapDimsDoc.x, -80.0) << phase << ": tile off left";
    EXPECT_GT(displayOffsetDoc.y + tile.bitmapDimsDoc.y, -80.0) << phase << ": tile off top";
    EXPECT_LT(displayOffsetDoc.x, kViewBoxWidth + 80.0) << phase << ": tile off right";
    EXPECT_LT(displayOffsetDoc.y, kViewBoxHeight + 80.0) << phase << ": tile off bottom";
  }

  EXPECT_GE(nonEmptyTiles, 3) << phase << ": stress scene did not produce enough layer tiles";
}

class EditorLayerStressTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(app_.loadFromString(kLayerStressSvg));
    source_ = std::string(kLayerStressSvg);
    selectTool_.setCompositedDragPreviewEnabled(true);
    SetCanvasSize(Vector2i(240, 120));
  }

  void SetCanvasSize(const Vector2i& canvasSize) {
    canvasSize_ = canvasSize;
    app_.document().document().setCanvasSize(canvasSize.x, canvasSize.y);
  }

  RenderResult RenderPhase(std::string_view phase, bool repostWhileBusy = false) {
    auto result = RequestRenderAndWait(asyncRenderer_, renderer_, app_, selectTool_, ++version_,
                                       phase, repostWhileBusy);
    EXPECT_FALSE(asyncRenderer_.isBusy()) << phase << ": worker stayed busy after result";
    EXPECT_TRUE(result.has_value()) << phase;
    return std::move(*result);
  }

  void ExpectSettledMatchesReference(std::string_view phase) {
    RenderResult result = RenderPhase(phase);
    EXPECT_EQ(result.stage, RenderResult::Stage::Final);
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
    EXPECT_EQ(result.stage, RenderResult::Stage::Final);
    EXPECT_LT(result.workerMs, 1500.0) << phase << ": active drag render exceeded budget";
    ExpectCompositedTileGeometryCoherent(result, canvasSize_, phase);
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

}  // namespace
}  // namespace donner::editor
