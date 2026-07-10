#include "donner/editor/RenderPanePresenter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <vector>

#include "donner/editor/ImGuiIncludes.h"
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnontrivial-memcall"
#endif
#include "imgui_internal.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include "donner/editor/PresentedFrameComposer.h"

namespace donner::editor {

namespace {

constexpr float kFrameBudget120Ms = 1000.0f / 120.0f;
constexpr float kFrameBudget60Ms = 1000.0f / 60.0f;
constexpr float kFrameGraphScaleMs = kFrameBudget60Ms * 2.0f;
constexpr float kFrameGraphWidth = 240.0f;
constexpr float kFrameGraphHeight = 32.0f;
constexpr float kMemoryGraphHeight = 28.0f;
constexpr std::uint64_t kBytesPerKiB = 1024u;
constexpr std::uint64_t kBytesPerMiB = 1024u * 1024u;
constexpr std::uint64_t kMinimumMemoryGraphScaleBytes = 64u * kBytesPerMiB;
/// Screen-edge margin shared by the FPS pill and the full graph.
constexpr float kPerfOverlayMargin = 8.0f;
// Frame-cost bucket colors, stacked bottom to top: main render, remaining UI
// work, host frame begin/end, unprofiled remainder. Muted categorical palette
// picked for adjacent-pair color-vision-deficiency separation against the
// dark graph surface; "other" is deliberately neutral gray because it
// aggregates unprofiled time.
constexpr ImU32 kFrameGraphRenderColor = IM_COL32(57, 135, 229, 255);
constexpr ImU32 kFrameGraphUiColor = IM_COL32(25, 158, 112, 255);
constexpr ImU32 kFrameGraphHostColor = IM_COL32(201, 133, 0, 255);
constexpr ImU32 kFrameGraphOtherColor = IM_COL32(137, 135, 129, 255);
constexpr ImU32 kFrameGraphBudget120Color = IM_COL32(255, 190, 82, 145);
constexpr ImU32 kFrameGraphBudget60Color = IM_COL32(255, 255, 255, 90);
constexpr ImU32 kFrameGraphMiss120Color = IM_COL32(255, 176, 48, 230);
constexpr ImU32 kFrameGraphMiss60Color = IM_COL32(220, 60, 60, 235);
// Memory bucket colors from the same muted palette: tile textures (active +
// overview), retired textures awaiting aging, and all other tracked bytes.
constexpr ImU32 kMemoryTilesColor = IM_COL32(144, 133, 233, 255);
constexpr ImU32 kMemoryRetiredColor = IM_COL32(201, 133, 0, 255);
constexpr ImU32 kMemoryOtherColor = IM_COL32(137, 135, 129, 255);
constexpr ImU32 kMemoryPeakColor = IM_COL32(255, 255, 255, 90);
ImVec4 ImU32ToImVec4(ImU32 color) {
  return ImGui::ColorConvertU32ToFloat4(color);
}

bool DragPreviewContainsEntity(const SelectTool::ActiveDragPreview& preview, Entity entity) {
  if (entity == preview.entity) {
    return true;
  }
  return std::ranges::find(preview.extraEntities, entity) != preview.extraEntities.end();
}

PresentedFrameTileGeometry PresentedGeometryFromTile(const GlTextureCache::TileView& tile);

PresentedFrameTileGeometry PresentedGeometryFromTileForActiveDrag(
    const GlTextureCache::TileView& tile,
    const std::optional<SelectTool::ActiveDragPreview>& activeDragPreview) {
  PresentedFrameTileGeometry geometry = PresentedGeometryFromTile(tile);
  if (TileMatchesActiveDragPreview(tile, activeDragPreview)) {
    geometry.isDragTarget = true;
  }
  return geometry;
}

void DrawProfilerLegendItem(ImU32 color, const char* label, bool sameLine) {
  if (sameLine) {
    ImGui::SameLine();
  }
  ImGui::PushStyleColor(ImGuiCol_Text, ImU32ToImVec4(color));
  ImGui::TextUnformatted(label);
  ImGui::PopStyleColor();
}

void DrawStackSegment(ImDrawList* dl, const ImVec2& barTopLeft, float barWidth, float graphHeight,
                      float scaleMs, float* stackedMs, float segmentMs, ImU32 color) {
  if (segmentMs <= 0.0f || scaleMs <= 0.0f) {
    return;
  }

  const float y0 = barTopLeft.y + graphHeight - (*stackedMs / scaleMs) * graphHeight;
  *stackedMs += segmentMs;
  const float y1 =
      barTopLeft.y + graphHeight - (std::min(*stackedMs, scaleMs) / scaleMs) * graphHeight;
  if (y1 < y0) {
    dl->AddRectFilled(ImVec2(barTopLeft.x, y1), ImVec2(barTopLeft.x + barWidth, y0), color);
  }
}

// The frame graph groups the profiled per-phase costs into a few buckets so
// the biggest costs read at a glance: main-thread document/pane rendering,
// the remaining main-thread UI work, host frame begin/end (surface acquire,
// uploads, ImGui draw, present), and the unprofiled remainder.
float MainRenderMs(const FrameProfilerSample& profiler) {
  return profiler.mainSourcePaneMs + profiler.mainRenderPaneMs + profiler.mainSidebarsMs;
}

float MainUiMs(const FrameProfilerSample& profiler) {
  return profiler.mainPreparationMs + profiler.mainRenderPollMs + profiler.mainDocumentFlushMs +
         profiler.mainOverlayRefreshMs + profiler.mainDocumentSyncMs + profiler.mainLayoutMs +
         profiler.mainShortcutsMs + profiler.mainMenusDialogsMs + profiler.mainSplittersMs +
         profiler.mainEndRenderRequestMs;
}

float HostMs(const FrameProfilerSample& profiler) {
  return profiler.hostBeginFrameMs + profiler.hostPreviousEndFrameMs;
}

ImU32 FrameBudgetMissColor(float frameMs) {
  if (frameMs > kFrameBudget60Ms) {
    return kFrameGraphMiss60Color;
  }

  if (frameMs > kFrameBudget120Ms) {
    return kFrameGraphMiss120Color;
  }

  return 0;
}

ImU32 FrameReadoutColor(float frameMs) {
  if (frameMs > kFrameBudget60Ms) {
    return kFrameGraphMiss60Color;
  }

  if (frameMs > kFrameBudget120Ms) {
    return kFrameGraphMiss120Color;
  }

  return IM_COL32(255, 255, 255, 255);
}

void DrawFrameBudgetLine(ImDrawList* dl, const ImVec2& origin, const ImVec2& bottomRight,
                         float budgetMs, ImU32 color) {
  const float y = origin.y + kFrameGraphHeight -
                  (std::min(budgetMs, kFrameGraphScaleMs) / kFrameGraphScaleMs) * kFrameGraphHeight;
  dl->AddLine(ImVec2(origin.x, y), ImVec2(bottomRight.x, y), color, 1.0f);
}

float BytesToMiB(std::uint64_t bytes) {
  return static_cast<float>(static_cast<double>(bytes) / static_cast<double>(kBytesPerMiB));
}

float BytesToKiB(std::uint64_t bytes) {
  return static_cast<float>(static_cast<double>(bytes) / static_cast<double>(kBytesPerKiB));
}

std::uint64_t RoundMemoryGraphScale(std::uint64_t bytes) {
  constexpr std::uint64_t kScaleStepBytes = 64u * kBytesPerMiB;
  const std::uint64_t clamped = std::max(bytes, kMinimumMemoryGraphScaleBytes);
  return ((clamped + kScaleStepBytes - 1u) / kScaleStepBytes) * kScaleStepBytes;
}

std::uint64_t MemoryGraphScaleBytes(const FrameHistory& history) {
  std::uint64_t maxBytes = 0;
  for (std::size_t i = 0; i < history.samples; ++i) {
    maxBytes = std::max(maxBytes, history.memory[i].totalTrackedBytes);
    maxBytes = std::max(maxBytes, history.memory[i].peakTrackedBytes);
  }
  return RoundMemoryGraphScale(maxBytes);
}

void DrawMemoryStackSegment(ImDrawList* dl, const ImVec2& barTopLeft, float barWidth,
                            float graphHeight, std::uint64_t scaleBytes,
                            std::uint64_t* stackedBytes, std::uint64_t segmentBytes, ImU32 color) {
  if (segmentBytes == 0u || scaleBytes == 0u) {
    return;
  }

  const auto yForBytes = [&](std::uint64_t bytes) {
    const double fraction =
        static_cast<double>(std::min(bytes, scaleBytes)) / static_cast<double>(scaleBytes);
    return static_cast<float>(barTopLeft.y + graphHeight - fraction * graphHeight);
  };
  const float y0 = yForBytes(*stackedBytes);
  *stackedBytes += segmentBytes;
  const float y1 = yForBytes(*stackedBytes);
  if (y1 < y0) {
    dl->AddRectFilled(ImVec2(barTopLeft.x, y1), ImVec2(barTopLeft.x + barWidth, y0), color);
  }
}

void RenderMemoryReadout(const FrameMemorySample& latest) {
  const std::uint64_t largest = std::max(latest.totalTrackedBytes, latest.peakTrackedBytes);
  if (largest > 0u && largest < kBytesPerMiB) {
    ImGui::Text("mem/peak %.0f / %.0f KiB", BytesToKiB(latest.totalTrackedBytes),
                BytesToKiB(latest.peakTrackedBytes));
    return;
  }

  ImGui::Text("mem/peak %.1f / %.1f MiB", BytesToMiB(latest.totalTrackedBytes),
              BytesToMiB(latest.peakTrackedBytes));
}

/// Smoothed frame-timing readout shared by the FPS pill and the full graph.
struct FrameReadout {
  float frameMs = 0.0f;
  float fps = 0.0f;
  float workerMs = 0.0f;
};

FrameReadout UpdateFrameReadout(const FrameHistory& history) {
  static float msEma = 0.0f;
  static double lastDisplayUpdateTime = 0.0;
  static float displayedMs = 0.0f;
  static float workerMsEma = 0.0f;
  static float displayedWorkerMs = 0.0f;

  const float latestMs = history.latest();
  msEma = msEma == 0.0f ? latestMs : 0.9f * msEma + 0.1f * latestMs;
  // Feed the worker EMA only from non-zero samples - between drag bursts we go
  // idle and stop emitting worker results; smoothing zero into the EMA would
  // collapse the readout to near-zero even though the last actual render was
  // 5-10 ms. Keep the EMA "sticky" at the most recent real measurement
  // instead.
  if (history.lastBackendMs > 0.0f) {
    workerMsEma = workerMsEma == 0.0f ? history.lastBackendMs
                                      : 0.9f * workerMsEma + 0.1f * history.lastBackendMs;
  }
  const double now = ImGui::GetTime();
  if (displayedMs == 0.0f || now - lastDisplayUpdateTime > 0.25) {
    displayedMs = msEma;
    displayedWorkerMs = workerMsEma;
    lastDisplayUpdateTime = now;
  }

  return FrameReadout{
      .frameMs = displayedMs,
      .fps = displayedMs > 0.0f ? 1000.0f / displayedMs : 0.0f,
      .workerMs = displayedWorkerMs,
  };
}

void RenderFpsPill(const FrameHistory& history, const Vector2d& contentRegion) {
  const FrameReadout readout = UpdateFrameReadout(history);
  char text[32];
  std::snprintf(text, sizeof(text), "%.0f FPS  %.1f ms", readout.fps, readout.frameMs);

  constexpr float kPillPaddingX = 8.0f;
  constexpr float kPillPaddingY = 3.0f;
  const ImVec2 textSize = ImGui::CalcTextSize(text);
  const ImVec2 pillSize(textSize.x + 2.0f * kPillPaddingX, textSize.y + 2.0f * kPillPaddingY);
  ImGui::SetCursorPos(
      ImVec2(static_cast<float>(contentRegion.x) - pillSize.x - kPerfOverlayMargin,
             static_cast<float>(contentRegion.y) - pillSize.y - kPerfOverlayMargin));
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(origin, ImVec2(origin.x + pillSize.x, origin.y + pillSize.y),
                    IM_COL32(30, 30, 30, 215), pillSize.y * 0.5f);
  dl->AddText(ImVec2(origin.x + kPillPaddingX, origin.y + kPillPaddingY),
              FrameReadoutColor(readout.frameMs), text);
  ImGui::Dummy(pillSize);
}

void RenderFrameGraph(const FrameHistory& history) {
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const FrameReadout readout = UpdateFrameReadout(history);

  const ImVec2 bottomRight(origin.x + kFrameGraphWidth, origin.y + kFrameGraphHeight);
  dl->AddRectFilled(origin, bottomRight, IM_COL32(30, 30, 30, 255));

  const float barWidth = kFrameGraphWidth / static_cast<float>(kFrameHistoryCapacity);
  for (std::size_t i = 0; i < history.samples; ++i) {
    const std::size_t readIdx =
        (history.writeIndex + kFrameHistoryCapacity - history.samples + i) % kFrameHistoryCapacity;
    const float ms = history.deltaMs[readIdx];
    const float x = origin.x + static_cast<float>(i) * barWidth;
    const FrameProfilerSample& profiler = history.profiler[readIdx];
    const float otherMs = std::max(0.0f, ms - profiler.totalProfiledMs());
    float stackedMs = 0.0f;
    const ImVec2 barOrigin(x, origin.y);
    DrawStackSegment(dl, barOrigin, barWidth, kFrameGraphHeight, kFrameGraphScaleMs, &stackedMs,
                     MainRenderMs(profiler), kFrameGraphRenderColor);
    DrawStackSegment(dl, barOrigin, barWidth, kFrameGraphHeight, kFrameGraphScaleMs, &stackedMs,
                     MainUiMs(profiler), kFrameGraphUiColor);
    DrawStackSegment(dl, barOrigin, barWidth, kFrameGraphHeight, kFrameGraphScaleMs, &stackedMs,
                     HostMs(profiler), kFrameGraphHostColor);
    DrawStackSegment(dl, barOrigin, barWidth, kFrameGraphHeight, kFrameGraphScaleMs, &stackedMs,
                     otherMs, kFrameGraphOtherColor);
    const ImU32 missColor = FrameBudgetMissColor(ms);
    if (missColor != 0) {
      dl->AddRectFilled(ImVec2(x, origin.y), ImVec2(x + barWidth, origin.y + 2.0f), missColor);
    }
  }

  DrawFrameBudgetLine(dl, origin, bottomRight, kFrameBudget120Ms, kFrameGraphBudget120Color);
  DrawFrameBudgetLine(dl, origin, bottomRight, kFrameBudget60Ms, kFrameGraphBudget60Color);

  // Async worker/presentation time overlay. Only non-zero samples are plotted -
  // zero means "no render result landed this frame" and we don't want a visible
  // drop-to-zero between drag bursts. A thin cyan line segment connects
  // consecutive non-zero samples; isolated points render as a single-pixel dot
  // so a one-off render still shows up.
  constexpr ImU32 kBackendColor = IM_COL32(0, 220, 240, 220);
  const auto backendY = [&](float ms) {
    const float clamped = std::min(ms, kFrameGraphScaleMs);
    return origin.y + kFrameGraphHeight - (clamped / kFrameGraphScaleMs) * kFrameGraphHeight;
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
      // Isolated sample - draw a small dot so it's visible.
      dl->AddRectFilled(ImVec2(point.x - 0.5f, point.y - 0.5f),
                        ImVec2(point.x + 1.0f, point.y + 1.0f), kBackendColor);
    }
    prev = point;
    havePrev = true;
  }

  ImGui::Dummy(ImVec2(kFrameGraphWidth, kFrameGraphHeight));
  const ImU32 textColor = FrameReadoutColor(readout.frameMs);
  ImGui::PushStyleColor(ImGuiCol_Text, textColor);
  ImGui::Text("%.2f ms / %.1f FPS", readout.frameMs, readout.fps);
  ImGui::PopStyleColor();
  if (readout.workerMs > 0.0f) {
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, kBackendColor);
    ImGui::Text("  worker %.2f ms", readout.workerMs);
    ImGui::PopStyleColor();
  }
  DrawProfilerLegendItem(kFrameGraphRenderColor, "render", /*sameLine=*/false);
  DrawProfilerLegendItem(kFrameGraphUiColor, "ui", /*sameLine=*/true);
  DrawProfilerLegendItem(kFrameGraphHostColor, "host", /*sameLine=*/true);
  DrawProfilerLegendItem(kFrameGraphOtherColor, "other", /*sameLine=*/true);
}

void RenderMemoryGraph(const FrameHistory& history) {
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 bottomRight(origin.x + kFrameGraphWidth, origin.y + kMemoryGraphHeight);
  dl->AddRectFilled(origin, bottomRight, IM_COL32(30, 30, 30, 255));

  const std::uint64_t scaleBytes = MemoryGraphScaleBytes(history);
  const float barWidth = kFrameGraphWidth / static_cast<float>(kFrameHistoryCapacity);
  for (std::size_t i = 0; i < history.samples; ++i) {
    const std::size_t readIdx =
        (history.writeIndex + kFrameHistoryCapacity - history.samples + i) % kFrameHistoryCapacity;
    const FrameMemorySample& memory = history.memory[readIdx];
    const float x = origin.x + static_cast<float>(i) * barWidth;
    const ImVec2 barOrigin(x, origin.y);
    std::uint64_t stackedBytes = 0;
    const std::uint64_t tileBytes = memory.activeTileBytes + memory.overviewTileBytes;
    DrawMemoryStackSegment(dl, barOrigin, barWidth, kMemoryGraphHeight, scaleBytes, &stackedBytes,
                           tileBytes, kMemoryTilesColor);
    DrawMemoryStackSegment(dl, barOrigin, barWidth, kMemoryGraphHeight, scaleBytes, &stackedBytes,
                           memory.retiredBytes, kMemoryRetiredColor);
    const std::uint64_t knownBytes = tileBytes + memory.retiredBytes;
    if (memory.totalTrackedBytes > knownBytes) {
      DrawMemoryStackSegment(dl, barOrigin, barWidth, kMemoryGraphHeight, scaleBytes, &stackedBytes,
                             memory.totalTrackedBytes - knownBytes, kMemoryOtherColor);
    }
  }

  const FrameMemorySample latest = history.latestNonZeroMemorySample();
  if (latest.peakTrackedBytes > 0u) {
    const double fraction = static_cast<double>(std::min(latest.peakTrackedBytes, scaleBytes)) /
                            static_cast<double>(scaleBytes);
    const float peakY =
        static_cast<float>(origin.y + kMemoryGraphHeight - fraction * kMemoryGraphHeight);
    dl->AddLine(ImVec2(origin.x, peakY), ImVec2(bottomRight.x, peakY), kMemoryPeakColor, 1.0f);
  }

  ImGui::Dummy(ImVec2(kFrameGraphWidth, kMemoryGraphHeight));
  RenderMemoryReadout(latest);
  DrawProfilerLegendItem(kMemoryTilesColor, "tiles", /*sameLine=*/false);
  DrawProfilerLegendItem(kMemoryRetiredColor, "retired", /*sameLine=*/true);
  DrawProfilerLegendItem(kMemoryOtherColor, "other", /*sameLine=*/true);
}

void DrawCheckerboard(ImDrawList* drawList, const ImVec2& topLeft, const ImVec2& bottomRight) {
  constexpr float kCheckerSize = static_cast<float>(kRenderPaneCheckerboardSize);
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

ImVec2 ToImVec2(const Vector2d& value) {
  return ImVec2(static_cast<float>(value.x), static_cast<float>(value.y));
}

bool IsFinite(const Vector2d& value) {
  return std::isfinite(value.x) && std::isfinite(value.y);
}

Box2d PresentedTileQuadBounds(const PresentedTileQuad& tileQuad) {
  Box2d bounds = Box2d::CreateEmpty(tileQuad.topLeft);
  bounds.addPoint(tileQuad.topRight);
  bounds.addPoint(tileQuad.bottomRight);
  bounds.addPoint(tileQuad.bottomLeft);
  return bounds;
}

ImU32 CompositorTileOverlayColor(const GlTextureCache::TileView& tile) {
  if (tile.isDragTarget) {
    return IM_COL32(255, 255, 255, tile.metadataOnly ? 150 : 245);
  }

  const int alpha = tile.metadataOnly ? 120 : 230;
  switch (tile.kind) {
    case RenderResult::CompositedTile::Kind::Segment: return IM_COL32(42, 214, 196, alpha);
    case RenderResult::CompositedTile::Kind::Layer: return IM_COL32(255, 178, 66, alpha);
    case RenderResult::CompositedTile::Kind::Immediate: return IM_COL32(255, 92, 138, alpha);
  }
  return IM_COL32(220, 220, 220, alpha);
}

char CompositorTileKindLabel(RenderResult::CompositedTile::Kind kind) {
  switch (kind) {
    case RenderResult::CompositedTile::Kind::Segment: return 'S';
    case RenderResult::CompositedTile::Kind::Layer: return 'L';
    case RenderResult::CompositedTile::Kind::Immediate: return 'I';
  }
  return '?';
}

void DrawCompositorTileOverlay(ImDrawList* drawList, const GlTextureCache::TileView& tile,
                               const PresentedTileQuad& tileQuad) {
  const ImU32 color = CompositorTileOverlayColor(tile);
  const std::array<ImVec2, 4> points = {ToImVec2(tileQuad.topLeft), ToImVec2(tileQuad.topRight),
                                        ToImVec2(tileQuad.bottomRight),
                                        ToImVec2(tileQuad.bottomLeft)};
  drawList->AddPolyline(points.data(), static_cast<int>(points.size()), color, ImDrawFlags_Closed,
                        tile.isDragTarget ? 2.0f : 1.5f);

  const Box2d bounds = PresentedTileQuadBounds(tileQuad);
  if (bounds.width() < 44.0 || bounds.height() < ImGui::GetTextLineHeight() + 6.0) {
    return;
  }

  char label[64];
  std::snprintf(label, sizeof(label), "%c %.24s g%llu%s", CompositorTileKindLabel(tile.kind),
                tile.id.c_str(), static_cast<unsigned long long>(tile.generation),
                tile.metadataOnly ? " cached" : "");
  const ImVec2 labelOrigin(static_cast<float>(bounds.topLeft.x + 4.0),
                           static_cast<float>(bounds.topLeft.y + 3.0));
  const ImVec2 labelSize = ImGui::CalcTextSize(label);
  drawList->AddRectFilled(
      ImVec2(labelOrigin.x - 2.0f, labelOrigin.y - 1.0f),
      ImVec2(labelOrigin.x + labelSize.x + 2.0f, labelOrigin.y + labelSize.y + 1.0f),
      IM_COL32(20, 22, 26, 210), 2.0f);
  drawList->AddText(labelOrigin, color, label);
}

}  // namespace

bool ShouldPresentCompositedTile(const GlTextureCache::TileView& tile, Entity suppressedLayerEntity,
                                 bool suppressDragTargetTiles) {
  if (tile.texture == 0) {
    return false;
  }

  if (suppressDragTargetTiles && tile.isDragTarget) {
    return false;
  }

  if (suppressedLayerEntity != entt::null && tile.layerEntity == suppressedLayerEntity) {
    return false;
  }

  return true;
}

bool TileMatchesActiveDragPreview(
    const GlTextureCache::TileView& tile,
    const std::optional<SelectTool::ActiveDragPreview>& activeDragPreview) {
  if (!activeDragPreview.has_value()) {
    return false;
  }
  if (tile.layerEntity != entt::null) {
    // isDragTarget is metadata from the worker frame that produced this tile.
    // It may outlive a group-to-child selection change, so entity identity is
    // authoritative for a new live drag.
    return DragPreviewContainsEntity(*activeDragPreview, tile.layerEntity);
  }
  return tile.isDragTarget;
}

bool HasPresentableDragTargetTile(
    const GlTextureCache& textures,
    const std::optional<SelectTool::ActiveDragPreview>& activeDragPreview,
    Entity suppressedLayerEntity, bool suppressDragTargetTiles) {
  if (!activeDragPreview.has_value()) {
    return false;
  }

  const auto matchesActiveDragTarget = [&](const GlTextureCache::TileView& tile) {
    if (suppressDragTargetTiles && TileMatchesActiveDragPreview(tile, activeDragPreview)) {
      return false;
    }
    return TileMatchesActiveDragPreview(tile, activeDragPreview) &&
           ShouldPresentCompositedTile(tile, suppressedLayerEntity, suppressDragTargetTiles);
  };

  if (std::ranges::any_of(textures.tiles(), matchesActiveDragTarget)) {
    return true;
  }

  return textures.activeTilesViewportBounded() &&
         std::ranges::any_of(textures.overviewTiles(), matchesActiveDragTarget);
}

bool ShouldPresentOverviewTiles(bool activeTilesViewportBounded,
                                std::span<const GlTextureCache::TileView> overviewTiles) {
  return activeTilesViewportBounded && !overviewTiles.empty();
}

bool PresentedTileQuadIntersectsScreenRect(const PresentedTileQuad& tileQuad,
                                           const Box2d& screenRect) {
  if (!IsFinite(tileQuad.topLeft) || !IsFinite(tileQuad.topRight) ||
      !IsFinite(tileQuad.bottomRight) || !IsFinite(tileQuad.bottomLeft) ||
      !IsFinite(screenRect.topLeft) || !IsFinite(screenRect.bottomRight)) {
    return false;
  }
  if (screenRect.bottomRight.x <= screenRect.topLeft.x ||
      screenRect.bottomRight.y <= screenRect.topLeft.y) {
    return false;
  }

  const Box2d tileBounds = PresentedTileQuadBounds(tileQuad);
  return tileBounds.bottomRight.x > screenRect.topLeft.x &&
         tileBounds.topLeft.x < screenRect.bottomRight.x &&
         tileBounds.bottomRight.y > screenRect.topLeft.y &&
         tileBounds.topLeft.y < screenRect.bottomRight.y;
}

std::optional<Box2d> PresentedImageClipRect(const Box2d& paneRect, const Box2d& imageRect) {
  if (!IsFinite(paneRect.topLeft) || !IsFinite(paneRect.bottomRight) ||
      !IsFinite(imageRect.topLeft) || !IsFinite(imageRect.bottomRight)) {
    return std::nullopt;
  }

  const Box2d clipRect(Vector2d(std::max(paneRect.topLeft.x, imageRect.topLeft.x),
                                std::max(paneRect.topLeft.y, imageRect.topLeft.y)),
                       Vector2d(std::min(paneRect.bottomRight.x, imageRect.bottomRight.x),
                                std::min(paneRect.bottomRight.y, imageRect.bottomRight.y)));
  if (clipRect.bottomRight.x <= clipRect.topLeft.x ||
      clipRect.bottomRight.y <= clipRect.topLeft.y) {
    return std::nullopt;
  }

  return clipRect;
}

void RenderPanePresenter::render(const RenderPanePresenterState& state) const {
  const bool hasVisibleTiles =
      std::ranges::any_of(state.textures.tiles(), [&](const GlTextureCache::TileView& tile) {
        return ShouldPresentCompositedTile(tile, state.suppressedLayerEntity,
                                           state.suppressDragTargetTiles);
      });
  const bool drawOverviewTiles = ShouldPresentOverviewTiles(
      state.textures.activeTilesViewportBounded(), state.textures.overviewTiles());
  const bool hasVisibleOverviewTiles =
      drawOverviewTiles &&
      std::ranges::any_of(state.textures.overviewTiles(),
                          [&](const GlTextureCache::TileView& tile) {
                            return ShouldPresentCompositedTile(tile, state.suppressedLayerEntity,
                                                               state.suppressDragTargetTiles);
                          });
  // Selection chrome (path outlines, hover, AABBs, oriented bounds, handles,
  // marquee) is rendered exclusively by Donner's OverlayRenderer straight onto
  // the Geode framebuffer (see EditorShell::DrawImmediateOverlaySnapshotToFramebuffer).
  // The presenter only blits composited document tiles plus UI furniture, so
  // presentable content here is purely tile-driven.
  const bool hasPresentedContent = hasVisibleTiles || hasVisibleOverviewTiles;

  const Box2d paneRect = Box2d::FromXYWH(state.viewport.paneOrigin.x, state.viewport.paneOrigin.y,
                                         state.viewport.paneSize.x, state.viewport.paneSize.y);
  if (paneRect.bottomRight.x <= paneRect.topLeft.x ||
      paneRect.bottomRight.y <= paneRect.topLeft.y) {
    return;
  }
  const Box2d screenRect = state.viewport.imageScreenRect();
  const std::optional<Box2d> imageClipRect = PresentedImageClipRect(paneRect, screenRect);
  const ImVec2 imageOrigin(static_cast<float>(screenRect.topLeft.x),
                           static_cast<float>(screenRect.topLeft.y));
  const ImVec2 imageBottomRight(static_cast<float>(screenRect.bottomRight.x),
                                static_cast<float>(screenRect.bottomRight.y));
  ImDrawList* paneDrawList = ImGui::GetWindowDrawList();
  paneDrawList->PushClipRect(ToImVec2(paneRect.topLeft), ToImVec2(paneRect.bottomRight),
                             /*intersect_with_current_clip_rect=*/true);
  if (!state.documentPresentedDirectly) {
    DrawCheckerboard(paneDrawList, imageOrigin, imageBottomRight);
  }
  if (!hasPresentedContent && !state.documentPresentedDirectly) {
    paneDrawList->PopClipRect();
    return;
  }

  const double pxPerDoc = state.viewport.pixelsPerDocUnit();
  const Vector2d imageOriginScreen(imageOrigin.x, imageOrigin.y);
  const Transform2d screenFromCanvasTransform =
      Transform2d::Scale(pxPerDoc) * Transform2d::Translate(imageOriginScreen);
  const std::optional<PresentedDragBaseline> dragBaseline =
      PresentedBaselineFromSelectPreviews(state.activeDragPreview, state.displayedDragPreview);
  const auto computeTileQuad = [&](const GlTextureCache::TileView& tile) {
    if (!ShouldPresentCompositedTile(tile, state.suppressedLayerEntity,
                                     state.suppressDragTargetTiles)) {
      return std::optional<PresentedTileQuad>();
    }
    if (state.suppressDragTargetTiles &&
        TileMatchesActiveDragPreview(tile, state.activeDragPreview)) {
      return std::optional<PresentedTileQuad>();
    }
    const std::optional<PresentedTileQuad> tileQuad = ComputePresentedTileQuad(
        PresentedGeometryFromTileForActiveDrag(tile, state.activeDragPreview),
        screenFromCanvasTransform, dragBaseline);
    if (!tileQuad.has_value()) {
      return std::optional<PresentedTileQuad>();
    }
    if (!imageClipRect.has_value() ||
        !PresentedTileQuadIntersectsScreenRect(*tileQuad, *imageClipRect)) {
      return std::optional<PresentedTileQuad>();
    }
    return tileQuad;
  };
  const auto drawTile = [&](const GlTextureCache::TileView& tile) {
    const std::optional<PresentedTileQuad> tileQuad = computeTileQuad(tile);
    if (!tileQuad.has_value()) {
      return;
    }
    const ImVec2 uvTopLeft(0.0f, 0.0f);
    const ImVec2 uvBottomRight(static_cast<float>(tile.uvBottomRight.x),
                               static_cast<float>(tile.uvBottomRight.y));
    const Box2d tileBounds = PresentedTileQuadBounds(*tileQuad);
    paneDrawList->AddImage(tile.texture, ToImVec2(tileBounds.topLeft),
                           ToImVec2(tileBounds.bottomRight), uvTopLeft, uvBottomRight);
  };
  if (imageClipRect.has_value() && !state.documentPresentedDirectly) {
    paneDrawList->PushClipRect(ToImVec2(imageClipRect->topLeft),
                               ToImVec2(imageClipRect->bottomRight),
                               /*intersect_with_current_clip_rect=*/true);
    if (drawOverviewTiles) {
      std::vector<Box2d> activeTileBounds;
      activeTileBounds.reserve(state.textures.tiles().size());
      for (const auto& tile : state.textures.tiles()) {
        const std::optional<PresentedTileQuad> tileQuad = computeTileQuad(tile);
        if (tileQuad.has_value()) {
          activeTileBounds.push_back(PresentedTileQuadBounds(*tileQuad));
        }
        if (TileMatchesActiveDragPreview(tile, state.activeDragPreview)) {
          const std::optional<PresentedTileQuad> cachedTileQuad = ComputePresentedTileQuad(
              PresentedGeometryFromTile(tile), screenFromCanvasTransform, std::nullopt);
          if (cachedTileQuad.has_value() &&
              PresentedTileQuadIntersectsScreenRect(*cachedTileQuad, *imageClipRect)) {
            activeTileBounds.push_back(PresentedTileQuadBounds(*cachedTileQuad));
          }
        }
      }

      const std::vector<Box2d> overviewClipRects =
          SubtractPresentedTileBoundsFromClip(*imageClipRect, activeTileBounds);
      for (const Box2d& overviewClipRect : overviewClipRects) {
        paneDrawList->PushClipRect(ToImVec2(overviewClipRect.topLeft),
                                   ToImVec2(overviewClipRect.bottomRight),
                                   /*intersect_with_current_clip_rect=*/true);
        for (const auto& tile : state.textures.overviewTiles()) {
          drawTile(tile);
        }
        paneDrawList->PopClipRect();
      }
    }
    for (const auto& tile : state.textures.tiles()) {
      drawTile(tile);
    }
    if (state.compositorTileOverlay) {
      for (const auto& tile : state.textures.tiles()) {
        if (const std::optional<PresentedTileQuad> tileQuad = computeTileQuad(tile)) {
          DrawCompositorTileOverlay(paneDrawList, tile, *tileQuad);
        }
      }
    }
    paneDrawList->PopClipRect();
  }
  paneDrawList->PopClipRect();

  if (state.perfOverlayMode == PerfOverlayMode::FpsPill) {
    RenderFpsPill(state.frameHistory, state.contentRegion);
  } else if (state.perfOverlayMode == PerfOverlayMode::FullGraph) {
    const float graphHeight = kFrameGraphHeight + kMemoryGraphHeight +
                              4.0f * ImGui::GetTextLineHeightWithSpacing() + 4.0f;
    ImGui::SetCursorPos(
        ImVec2(kPerfOverlayMargin,
               static_cast<float>(state.contentRegion.y - graphHeight - kPerfOverlayMargin)));
    RenderFrameGraph(state.frameHistory);
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    RenderMemoryGraph(state.frameHistory);
  }
}

}  // namespace donner::editor
