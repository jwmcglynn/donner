#include "donner/editor/RenderPanePresenter.h"

#include <algorithm>
#include <cmath>

#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/PresentedFrameComposer.h"

namespace donner::editor {

namespace {

constexpr float kTargetFrameMs = 1000.0f / 60.0f;
/// Threshold above which a frame is flagged "over budget" (red bar /
/// red readout). We use 1.1x the 60 Hz target (~18.3 ms) rather than the
/// bare target so measurement noise near the 16.666 ms boundary doesn't
/// thrash the color back and forth between red and green every frame.
/// This is the "you actually missed vsync on a 60 Hz display" zone; the
/// 16.666 ms reference line is still drawn on the graph at the exact
/// 60 Hz target.
constexpr float kOverBudgetThresholdMs = kTargetFrameMs * 1.1f;
constexpr float kFrameGraphWidth = 240.0f;
constexpr float kFrameGraphHeight = 32.0f;

void RenderFrameGraph(const FrameHistory& history) {
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float latestMs = history.latest();
  static float msEma = 0.0f;
  static double lastDisplayUpdateTime = 0.0;
  static float displayedMs = 0.0f;
  static float backendMsEma = 0.0f;
  static float displayedBackendMs = 0.0f;

  msEma = msEma == 0.0f ? latestMs : 0.9f * msEma + 0.1f * latestMs;
  // Feed the worker EMA only from non-zero samples — between drag bursts we go
  // idle and stop emitting worker results; smoothing zero into the EMA would
  // collapse the readout to near-zero even though the last actual render was
  // 5-10 ms. Keep the EMA "sticky" at the most recent real measurement
  // instead.
  if (history.lastBackendMs > 0.0f) {
    backendMsEma = backendMsEma == 0.0f ? history.lastBackendMs
                                        : 0.9f * backendMsEma + 0.1f * history.lastBackendMs;
  }
  const double now = ImGui::GetTime();
  if (displayedMs == 0.0f || now - lastDisplayUpdateTime > 0.25) {
    displayedMs = msEma;
    displayedBackendMs = backendMsEma;
    lastDisplayUpdateTime = now;
  }
  const float displayedFps = displayedMs > 0.0f ? 1000.0f / displayedMs : 0.0f;

  const ImVec2 bottomRight(origin.x + kFrameGraphWidth, origin.y + kFrameGraphHeight);
  dl->AddRectFilled(origin, bottomRight, IM_COL32(30, 30, 30, 255));

  constexpr float scaleMs = kTargetFrameMs * 2.0f;
  const float budgetY =
      origin.y + kFrameGraphHeight - (kTargetFrameMs / scaleMs) * kFrameGraphHeight;
  dl->AddLine(ImVec2(origin.x, budgetY), ImVec2(bottomRight.x, budgetY),
              IM_COL32(255, 255, 255, 80), 1.0f);

  const float barWidth = kFrameGraphWidth / static_cast<float>(kFrameHistoryCapacity);
  for (std::size_t i = 0; i < history.samples; ++i) {
    const std::size_t readIdx =
        (history.writeIndex + kFrameHistoryCapacity - history.samples + i) % kFrameHistoryCapacity;
    const float ms = history.deltaMs[readIdx];
    const float clamped = std::min(ms, scaleMs);
    const float barHeight = (clamped / scaleMs) * kFrameGraphHeight;
    const bool overBudget = ms > kOverBudgetThresholdMs;
    const ImU32 color = overBudget ? IM_COL32(220, 60, 60, 255) : IM_COL32(80, 200, 100, 255);

    const float x = origin.x + static_cast<float>(i) * barWidth;
    dl->AddRectFilled(ImVec2(x, origin.y + kFrameGraphHeight - barHeight),
                      ImVec2(x + barWidth, origin.y + kFrameGraphHeight), color);
  }

  // Async worker/presentation time overlay. Only non-zero samples are plotted —
  // zero means "no render result landed this frame" and we don't want a visible
  // drop-to-zero between drag bursts. A thin cyan line segment connects
  // consecutive non-zero samples; isolated points render as a single-pixel dot
  // so a one-off render still shows up.
  constexpr ImU32 kBackendColor = IM_COL32(0, 220, 240, 220);
  const auto backendY = [&](float ms) {
    const float clamped = std::min(ms, scaleMs);
    return origin.y + kFrameGraphHeight - (clamped / scaleMs) * kFrameGraphHeight;
  };
  bool havePrev = false;
  ImVec2 prev(0.0f, 0.0f);
  for (std::size_t i = 0; i < history.samples; ++i) {
    const std::size_t readIdx =
        (history.writeIndex + kFrameHistoryCapacity - history.samples + i) % kFrameHistoryCapacity;
    const float backendMs = history.backendMs[readIdx];
    if (backendMs <= 0.0f) {
      // Treat the gap as a segment break so the line doesn't span the
      // idle stretch between drag bursts.
      havePrev = false;
      continue;
    }
    const float x = origin.x + (static_cast<float>(i) + 0.5f) * barWidth;
    const ImVec2 point(x, backendY(backendMs));
    if (havePrev) {
      dl->AddLine(prev, point, kBackendColor, 1.5f);
    } else {
      // Isolated sample — draw a small dot so it's visible.
      dl->AddRectFilled(ImVec2(point.x - 0.5f, point.y - 0.5f),
                        ImVec2(point.x + 1.0f, point.y + 1.0f), kBackendColor);
    }
    prev = point;
    havePrev = true;
  }

  ImGui::Dummy(ImVec2(kFrameGraphWidth, kFrameGraphHeight));
  const ImU32 textColor = displayedMs > kOverBudgetThresholdMs ? IM_COL32(220, 60, 60, 255)
                                                               : IM_COL32(255, 255, 255, 255);
  ImGui::PushStyleColor(ImGuiCol_Text, textColor);
  ImGui::Text("%.2f ms / %.1f FPS", displayedMs, displayedFps);
  ImGui::PopStyleColor();
  if (displayedBackendMs > 0.0f) {
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, kBackendColor);
    ImGui::Text("  worker %.2f ms", displayedBackendMs);
    ImGui::PopStyleColor();
  }
}

void DrawCheckerboard(ImDrawList* drawList, const ImVec2& topLeft, const ImVec2& bottomRight) {
  constexpr float kCheckerSize = 16.0f;
  const ImVec2 snappedTopLeft(std::floor(topLeft.x), std::floor(topLeft.y));
  const ImVec2 snappedBottomRight(std::floor(bottomRight.x), std::floor(bottomRight.y));
  if (snappedTopLeft.x >= snappedBottomRight.x || snappedTopLeft.y >= snappedBottomRight.y) {
    return;
  }

  drawList->PushClipRect(snappedTopLeft, snappedBottomRight,
                         /*intersect_with_current_clip_rect=*/true);
  const float startY = std::floor(snappedTopLeft.y / kCheckerSize) * kCheckerSize;
  const float startX = std::floor(snappedTopLeft.x / kCheckerSize) * kCheckerSize;
  for (float y = startY; y < snappedBottomRight.y; y += kCheckerSize) {
    const int row = static_cast<int>(std::floor(y / kCheckerSize));
    for (float x = startX; x < snappedBottomRight.x; x += kCheckerSize) {
      const int column = static_cast<int>(std::floor(x / kCheckerSize));
      const ImU32 color =
          ((row + column) % 2 == 0) ? IM_COL32(60, 60, 60, 255) : IM_COL32(40, 40, 40, 255);
      drawList->AddRectFilled(ImVec2(x, y), ImVec2(x + kCheckerSize, y + kCheckerSize), color);
    }
  }
  drawList->PopClipRect();
}

PresentedFrameTileGeometry PresentedGeometryFromTile(const GlTextureCache::TileView& tile) {
  return PresentedFrameTileGeometry{
      .canvasOffsetDoc = tile.canvasOffsetDoc,
      .bitmapDimsDoc = tile.bitmapDimsDoc,
      .dragTranslationDoc = tile.dragTranslationDoc,
      .documentFromCachedDocument = tile.documentFromCachedDocument,
      .isDragTarget = tile.isDragTarget,
  };
}

std::optional<PresentedDragBaseline> PresentedBaselineFromSelectPreviews(
    const std::optional<SelectTool::ActiveDragPreview>& activePreview,
    const std::optional<SelectTool::ActiveDragPreview>& displayedPreview) {
  if (!activePreview.has_value() || !displayedPreview.has_value() ||
      activePreview->entity != displayedPreview->entity ||
      activePreview->dragGeneration != displayedPreview->dragGeneration) {
    return std::nullopt;
  }

  return PresentedDragBaseline{
      .entity = activePreview->entity,
      .representedTranslationDoc = displayedPreview->translation,
      .activeTranslationDoc = activePreview->translation,
      .representedDocumentFromCachedDocument = displayedPreview->documentFromCachedDocument,
      .activeDocumentFromCachedDocument = activePreview->documentFromCachedDocument,
  };
}

std::optional<PresentedDragBaseline> TranslationOverlayBaselineFromSelectPreviews(
    const std::optional<SelectTool::ActiveDragPreview>& activePreview,
    const std::optional<SelectTool::ActiveDragPreview>& displayedPreview) {
  if (!activePreview.has_value() || !displayedPreview.has_value() ||
      !activePreview->documentFromCachedDocument.isTranslation() ||
      !displayedPreview->documentFromCachedDocument.isTranslation()) {
    return std::nullopt;
  }
  return PresentedBaselineFromSelectPreviews(activePreview, displayedPreview);
}

ImVec2 ToImVec2(const Vector2d& value) {
  return ImVec2(static_cast<float>(value.x), static_cast<float>(value.y));
}

}  // namespace

bool ShouldPresentCompositedTile(const GlTextureCache::TileView& tile,
                                 bool suppressDragTargetTiles) {
  if (tile.texture == 0) {
    return false;
  }

  return !(suppressDragTargetTiles && tile.isDragTarget);
}

void RenderPanePresenter::render(const RenderPanePresenterState& state) const {
  const bool hasVisibleTiles =
      std::ranges::any_of(state.textures.tiles(), [&](const GlTextureCache::TileView& tile) {
        return ShouldPresentCompositedTile(tile, state.suppressDragTargetTiles);
      });
  const bool hasOverlay = state.textures.overlayWidth() > 0 && state.textures.overlayHeight() > 0;
  if (!hasVisibleTiles && !hasOverlay) {
    ImGui::TextUnformatted("(no rendered image)");
    return;
  }

  const Box2d screenRect = state.viewport.imageScreenRect();
  const ImVec2 imageOrigin(static_cast<float>(screenRect.topLeft.x),
                           static_cast<float>(screenRect.topLeft.y));
  const ImVec2 imageBottomRight(static_cast<float>(screenRect.bottomRight.x),
                                static_cast<float>(screenRect.bottomRight.y));
  ImDrawList* paneDrawList = ImGui::GetWindowDrawList();
  DrawCheckerboard(paneDrawList, imageOrigin, imageBottomRight);

  const double pxPerDoc = state.viewport.pixelsPerDocUnit();
  const Vector2d imageOriginScreen(imageOrigin.x, imageOrigin.y);
  const Transform2d screenFromCanvasTransform =
      Transform2d::Scale(pxPerDoc) * Transform2d::Translate(imageOriginScreen);
  const std::optional<PresentedDragBaseline> dragBaseline =
      PresentedBaselineFromSelectPreviews(state.activeDragPreview, state.displayedDragPreview);
  bool hasDragTargetTile = false;
  for (const auto& tile : state.textures.tiles()) {
    if (!ShouldPresentCompositedTile(tile, state.suppressDragTargetTiles)) {
      continue;
    }
    hasDragTargetTile = hasDragTargetTile || tile.isDragTarget;
    const std::optional<PresentedTileQuad> tileQuad = ComputePresentedTileQuad(
        PresentedGeometryFromTile(tile), screenFromCanvasTransform, dragBaseline);
    if (!tileQuad.has_value()) {
      continue;
    }
    paneDrawList->AddImageQuad(tile.texture, ToImVec2(tileQuad->topLeft),
                               ToImVec2(tileQuad->topRight), ToImVec2(tileQuad->bottomRight),
                               ToImVec2(tileQuad->bottomLeft));
  }

  // All editor chrome — path outlines, selection AABBs, and the
  // marquee rect — is rasterized into `overlayTexture` by
  // `OverlayRenderer::drawChromeWithTransform` and uploaded once per
  // frame. The ImGui-direct `AddRect` / `AddRectFilled` chrome path
  // was removed so there is a single invalidation envelope the GPU
  // backend (Geode) can optimize end-to-end.
  if (state.textures.overlayWidth() > 0 && state.textures.overlayHeight() > 0) {
    const std::optional<PresentedDragBaseline> overlayBaseline =
        hasDragTargetTile ? TranslationOverlayBaselineFromSelectPreviews(state.activeDragPreview,
                                                                         state.overlayDragPreview)
                          : std::nullopt;
    const Transform2d documentFromOverlayDocument =
        ResolvePresentedOverlayDocumentTransform(overlayBaseline);
    const Vector2d docTopLeft = state.viewport.documentViewBox.topLeft;
    const Vector2d docTopRight(state.viewport.documentViewBox.bottomRight.x,
                               state.viewport.documentViewBox.topLeft.y);
    const Vector2d docBottomRight = state.viewport.documentViewBox.bottomRight;
    const Vector2d docBottomLeft(state.viewport.documentViewBox.topLeft.x,
                                 state.viewport.documentViewBox.bottomRight.y);
    const auto overlayPoint = [&](const Vector2d& documentPoint) {
      return state.viewport.documentToScreen(
          documentFromOverlayDocument.transformPosition(documentPoint));
    };
    paneDrawList->PushClipRect(imageOrigin, imageBottomRight,
                               /*intersect_with_current_clip_rect=*/true);
    paneDrawList->AddImageQuad(state.textures.overlayTexture(), ToImVec2(overlayPoint(docTopLeft)),
                               ToImVec2(overlayPoint(docTopRight)),
                               ToImVec2(overlayPoint(docBottomRight)),
                               ToImVec2(overlayPoint(docBottomLeft)));
    paneDrawList->PopClipRect();
  }

  constexpr float kFramePadding = 8.0f;
  const float graphHeight = kFrameGraphHeight + ImGui::GetTextLineHeightWithSpacing();
  ImGui::SetCursorPos(ImVec2(
      kFramePadding, static_cast<float>(state.contentRegion.y - graphHeight - kFramePadding)));
  RenderFrameGraph(state.frameHistory);
}

}  // namespace donner::editor
