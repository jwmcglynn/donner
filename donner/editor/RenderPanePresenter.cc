#include "donner/editor/RenderPanePresenter.h"

#include <cmath>

#include "donner/editor/ImGuiIncludes.h"

namespace donner::editor {

namespace {

constexpr float kTargetFrameMs = 1000.0f / 60.0f;
constexpr float kFrameGraphWidth = 240.0f;
constexpr float kFrameGraphHeight = 32.0f;
constexpr ImU32 kSelectionChromeColor = IM_COL32(0x00, 0xc8, 0xff, 0xff);
constexpr float kSelectionChromeThickness = 1.5f;
constexpr ImU32 kMarqueeFillColor = IM_COL32(0x00, 0xc8, 0xff, 0x33);
constexpr ImU32 kMarqueeStrokeColor = IM_COL32(0xff, 0xff, 0xff, 0xff);
constexpr float kMarqueeStrokeThickness = 1.5f;

Vector2d DragPreviewScreenOffset(const std::optional<SelectTool::ActiveDragPreview>& dragPreview,
                                 const ViewportState& viewport) {
  if (!dragPreview.has_value()) {
    return Vector2d::Zero();
  }

  return dragPreview->translation * viewport.pixelsPerDocUnit();
}

void RenderFrameGraph(const FrameHistory& history) {
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float latestMs = history.latest();
  static float msEma = 0.0f;
  static double lastDisplayUpdateTime = 0.0;
  static float displayedMs = 0.0f;

  msEma = msEma == 0.0f ? latestMs : 0.9f * msEma + 0.1f * latestMs;
  const double now = ImGui::GetTime();
  if (displayedMs == 0.0f || now - lastDisplayUpdateTime > 0.25) {
    displayedMs = msEma;
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
    const bool overBudget = ms > kTargetFrameMs;
    const ImU32 color = overBudget ? IM_COL32(220, 60, 60, 255) : IM_COL32(80, 200, 100, 255);

    const float x = origin.x + static_cast<float>(i) * barWidth;
    dl->AddRectFilled(ImVec2(x, origin.y + kFrameGraphHeight - barHeight),
                      ImVec2(x + barWidth, origin.y + kFrameGraphHeight), color);
  }

  ImGui::Dummy(ImVec2(kFrameGraphWidth, kFrameGraphHeight));
  const ImU32 textColor =
      displayedMs > kTargetFrameMs ? IM_COL32(220, 60, 60, 255) : IM_COL32(255, 255, 255, 255);
  ImGui::PushStyleColor(ImGuiCol_Text, textColor);
  ImGui::Text("%.2f ms / %.1f FPS", displayedMs, displayedFps);
  ImGui::PopStyleColor();
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

void RenderPanePresenter::render(const RenderPanePresenterState& state) const {
  const auto dragScreenOffset = DragPreviewScreenOffset(state.displayedDragPreview, state.viewport);

  if (state.textures.flatWidth() <= 0 && state.textures.flatHeight() <= 0 &&
      !state.experimentalDragPresentation.hasCachedTextures) {
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

  if (state.experimentalMode &&
      state.experimentalDragPresentation.shouldDisplayCompositedLayers(state.activeDragPreview) &&
      state.textures.promotedWidth() > 0 && state.textures.promotedHeight() > 0 &&
      state.displayedDragPreview.has_value()) {
    if (state.textures.backgroundWidth() > 0 && state.textures.backgroundHeight() > 0) {
      paneDrawList->AddImage(
          static_cast<ImTextureID>(static_cast<std::uintptr_t>(state.textures.backgroundTexture())),
          imageOrigin, imageBottomRight);
    }

    const ImVec2 promotedOrigin(imageOrigin.x + static_cast<float>(dragScreenOffset.x),
                                imageOrigin.y + static_cast<float>(dragScreenOffset.y));
    const ImVec2 promotedBottomRight(imageBottomRight.x + static_cast<float>(dragScreenOffset.x),
                                     imageBottomRight.y + static_cast<float>(dragScreenOffset.y));
    paneDrawList->AddImage(
        static_cast<ImTextureID>(static_cast<std::uintptr_t>(state.textures.promotedTexture())),
        promotedOrigin, promotedBottomRight);

    if (state.textures.foregroundWidth() > 0 && state.textures.foregroundHeight() > 0) {
      paneDrawList->AddImage(
          static_cast<ImTextureID>(static_cast<std::uintptr_t>(state.textures.foregroundTexture())),
          imageOrigin, imageBottomRight);
    }
  } else {
    paneDrawList->AddImage(
        static_cast<ImTextureID>(static_cast<std::uintptr_t>(state.textures.flatTexture())),
        imageOrigin, imageBottomRight);
  }

  if (state.textures.overlayWidth() > 0 && state.textures.overlayHeight() > 0) {
    const ImVec2 overlayOrigin(imageOrigin.x + static_cast<float>(dragScreenOffset.x),
                               imageOrigin.y + static_cast<float>(dragScreenOffset.y));
    const ImVec2 overlayBottomRight(imageBottomRight.x + static_cast<float>(dragScreenOffset.x),
                                    imageBottomRight.y + static_cast<float>(dragScreenOffset.y));
    paneDrawList->AddImage(
        static_cast<ImTextureID>(static_cast<std::uintptr_t>(state.textures.overlayTexture())),
        overlayOrigin, overlayBottomRight);
  }

  for (const Box2d& screenAabb :
       ComputeSelectionAabbScreenRects(state.viewport, state.selectionBoundsDoc)) {
    paneDrawList->AddRect(ImVec2(static_cast<float>(screenAabb.topLeft.x + dragScreenOffset.x),
                                 static_cast<float>(screenAabb.topLeft.y + dragScreenOffset.y)),
                          ImVec2(static_cast<float>(screenAabb.bottomRight.x + dragScreenOffset.x),
                                 static_cast<float>(screenAabb.bottomRight.y + dragScreenOffset.y)),
                          kSelectionChromeColor, 0.0f, ImDrawFlags_None, kSelectionChromeThickness);
  }

  if (state.marqueeRectDoc.has_value()) {
    const Box2d marqueeRectScreen = state.viewport.documentToScreen(*state.marqueeRectDoc);
    paneDrawList->AddRectFilled(ImVec2(static_cast<float>(marqueeRectScreen.topLeft.x),
                                       static_cast<float>(marqueeRectScreen.topLeft.y)),
                                ImVec2(static_cast<float>(marqueeRectScreen.bottomRight.x),
                                       static_cast<float>(marqueeRectScreen.bottomRight.y)),
                                kMarqueeFillColor);
    paneDrawList->AddRect(ImVec2(static_cast<float>(marqueeRectScreen.topLeft.x),
                                 static_cast<float>(marqueeRectScreen.topLeft.y)),
                          ImVec2(static_cast<float>(marqueeRectScreen.bottomRight.x),
                                 static_cast<float>(marqueeRectScreen.bottomRight.y)),
                          kMarqueeStrokeColor, 0.0f, ImDrawFlags_None, kMarqueeStrokeThickness);
  }

  constexpr float kFramePadding = 8.0f;
  const float graphHeight = kFrameGraphHeight + ImGui::GetTextLineHeightWithSpacing();
  ImGui::SetCursorPos(ImVec2(
      kFramePadding, static_cast<float>(state.contentRegion.y - graphHeight - kFramePadding)));
  RenderFrameGraph(state.frameHistory);
}

}  // namespace donner::editor
