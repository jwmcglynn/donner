#include "donner/editor/RenderPanePresenter.h"

#include <cmath>

#include "donner/editor/ImGuiIncludes.h"

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
  // Feed the backend EMA only from non-zero samples — between drag
  // bursts we go idle and stop emitting backend work; smoothing zero
  // into the EMA would collapse the readout to near-zero even though
  // the last actual render was 5-10 ms. Keep the EMA "sticky" at the
  // most recent real measurement instead.
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

  // Backend (async-renderer worker) time overlay. Only non-zero samples
  // are plotted — zero means "no render result landed this frame" and
  // we don't want a visible drop-to-zero between drag bursts. A thin
  // cyan line segment connects consecutive non-zero samples; isolated
  // points render as a single-pixel dot so a one-off render still
  // shows up.
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
    ImGui::Text("  backend %.2f ms", displayedBackendMs);
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

}  // namespace

Vector2d ResolveCompositedTileDragTranslation(
    const GlTextureCache::TileView& tile,
    const std::optional<SelectTool::ActiveDragPreview>& activeDragPreview,
    const std::optional<SelectTool::ActiveDragPreview>& displayedDragPreview) {
  if (tile.isDragTarget && activeDragPreview.has_value() && displayedDragPreview.has_value() &&
      displayedDragPreview->entity == activeDragPreview->entity) {
    return activeDragPreview->translation;
  }

  return tile.dragTranslationDoc;
}

void RenderPanePresenter::render(const RenderPanePresenterState& state) const {
  const bool hasFlat = state.textures.flatWidth() > 0 && state.textures.flatHeight() > 0;
  const bool hasTiles = !state.textures.tiles().empty();
  if (!hasFlat && !state.experimentalDragPresentation.hasCachedTextures && !hasTiles) {
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

  // M2C: when the worker has published a composited tile list and the
  // editor is in the experimental drag-presentation mode, blit the
  // tiles directly in paint order. Each tile carries its own canvas
  // offset (in doc units) and intrinsic dimensions, so the bitmap
  // tracks pinch-zoom changes via the live `pixelsPerDocUnit` (same
  // §M2B property the old single-promoted-bitmap path had, applied
  // uniformly now to every tile).
  const bool useTiles =
      state.experimentalMode && hasTiles &&
      state.experimentalDragPresentation.shouldDisplayCompositedLayers(state.activeDragPreview) &&
      state.displayedDragPreview.has_value();
  if (useTiles) {
    const double pxPerDoc = state.viewport.pixelsPerDocUnit();
    for (const auto& tile : state.textures.tiles()) {
      if (tile.texture == 0) {
        continue;
      }
      const Vector2d originDoc =
          tile.canvasOffsetDoc + ResolveCompositedTileDragTranslation(tile, state.activeDragPreview,
                                                                      state.displayedDragPreview);
      Vector2d sizeScreen = tile.bitmapDimsDoc * pxPerDoc;
      if (sizeScreen.x <= 0.0 || sizeScreen.y <= 0.0) {
        // Defensive fallback for canvas-sized tiles that pre-date
        // M2's intrinsic-size rasterize — stretch across the full
        // pane, matching the old bg/fg behavior.
        sizeScreen.x = static_cast<double>(imageBottomRight.x - imageOrigin.x);
        sizeScreen.y = static_cast<double>(imageBottomRight.y - imageOrigin.y);
      }
      const ImVec2 tileOrigin(imageOrigin.x + static_cast<float>(originDoc.x * pxPerDoc),
                              imageOrigin.y + static_cast<float>(originDoc.y * pxPerDoc));
      const ImVec2 tileBottomRight(tileOrigin.x + static_cast<float>(sizeScreen.x),
                                   tileOrigin.y + static_cast<float>(sizeScreen.y));
      paneDrawList->AddImage(static_cast<ImTextureID>(static_cast<std::uintptr_t>(tile.texture)),
                             tileOrigin, tileBottomRight);
    }
  } else if (hasFlat) {
    paneDrawList->AddImage(
        static_cast<ImTextureID>(static_cast<std::uintptr_t>(state.textures.flatTexture())),
        imageOrigin, imageBottomRight);
  }

  // All editor chrome — path outlines, selection AABBs, and the
  // marquee rect — is rasterized into `overlayTexture` by
  // `OverlayRenderer::drawChromeWithTransform` and uploaded once per
  // frame. The ImGui-direct `AddRect` / `AddRectFilled` chrome path
  // was removed so there is a single invalidation envelope the GPU
  // backend (Geode) can optimize end-to-end.
  if (state.textures.overlayWidth() > 0 && state.textures.overlayHeight() > 0) {
    paneDrawList->AddImage(
        static_cast<ImTextureID>(static_cast<std::uintptr_t>(state.textures.overlayTexture())),
        imageOrigin, imageBottomRight);
  }

  constexpr float kFramePadding = 8.0f;
  const float graphHeight = kFrameGraphHeight + ImGui::GetTextLineHeightWithSpacing();
  ImGui::SetCursorPos(ImVec2(
      kFramePadding, static_cast<float>(state.contentRegion.y - graphHeight - kFramePadding)));
  RenderFrameGraph(state.frameHistory);
}

}  // namespace donner::editor
