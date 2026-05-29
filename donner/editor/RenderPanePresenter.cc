#include "donner/editor/RenderPanePresenter.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <span>

#include "donner/editor/ImGuiIncludes.h"
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
constexpr ImU32 kFrameGraphOtherColor = IM_COL32(85, 140, 95, 255);
constexpr ImU32 kFrameGraphOverlayCaptureColor = IM_COL32(255, 205, 86, 255);
constexpr ImU32 kFrameGraphOverlayRenderColor = IM_COL32(255, 145, 61, 255);
constexpr ImU32 kFrameGraphOverlaySnapshotColor = IM_COL32(255, 112, 112, 255);
constexpr ImU32 kFrameGraphOverlayUploadColor = IM_COL32(212, 92, 255, 255);
constexpr ImU32 kFrameGraphCompositedUploadColor = IM_COL32(0, 220, 240, 255);
constexpr ImU32 kFrameGraphRenderImmediateColor = IM_COL32(255, 96, 96, 255);
constexpr ImU32 kFrameGraphRenderCachedColor = IM_COL32(70, 155, 255, 255);
constexpr ImU32 kFrameGraphSourceRopeColor = IM_COL32(117, 132, 255, 255);
constexpr ImU32 kFrameGraphBudget120Color = IM_COL32(255, 190, 82, 145);
constexpr ImU32 kFrameGraphBudget60Color = IM_COL32(255, 255, 255, 90);
constexpr ImU32 kFrameGraphMiss120Color = IM_COL32(255, 176, 48, 230);
constexpr ImU32 kFrameGraphMiss60Color = IM_COL32(220, 60, 60, 235);
constexpr ImU32 kMemoryOtherColor = IM_COL32(90, 98, 112, 255);
constexpr ImU32 kMemoryActiveTileColor = IM_COL32(0, 190, 215, 255);
constexpr ImU32 kMemoryOverviewTileColor = IM_COL32(78, 170, 116, 255);
constexpr ImU32 kMemoryRetiredColor = IM_COL32(215, 110, 64, 255);
constexpr ImU32 kMemoryOverlayColor = IM_COL32(212, 92, 255, 255);
constexpr ImU32 kMemoryPeakColor = IM_COL32(255, 255, 255, 90);
constexpr ImU32 kOverlaySelectionColor = IM_COL32(0x00, 0xc8, 0xff, 0xff);
constexpr ImU32 kOverlayDisplayNoneColor = IM_COL32(0x5f, 0x9a, 0xb2, 0xff);
constexpr ImU32 kOverlayHandleFillColor = IM_COL32(0xff, 0xff, 0xff, 0xff);
constexpr ImU32 kOverlaySourceHoverStrokeColor = IM_COL32(0xff, 0xff, 0xff, 0xd0);
constexpr ImU32 kOverlaySourceHoverBoundsColor = IM_COL32(0x00, 0xc8, 0xff, 0xc8);
constexpr ImU32 kOverlayMarqueeFillColor = IM_COL32(0x00, 0xc8, 0xff, 0x33);
constexpr ImU32 kOverlayMarqueeStrokeColor = IM_COL32(0xff, 0xff, 0xff, 0xff);

ImVec4 ImU32ToImVec4(ImU32 color) {
  return ImGui::ColorConvertU32ToFloat4(color);
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

  const float barWidth = kFrameGraphWidth / static_cast<float>(kFrameHistoryCapacity);
  for (std::size_t i = 0; i < history.samples; ++i) {
    const std::size_t readIdx =
        (history.writeIndex + kFrameHistoryCapacity - history.samples + i) % kFrameHistoryCapacity;
    const float ms = history.deltaMs[readIdx];
    const float x = origin.x + static_cast<float>(i) * barWidth;
    const FrameProfilerSample& profiler = history.profiler[readIdx];
    const float sourceRopesMs =
        profiler.sourceRopeLayoutMs + profiler.sourceRopeUpdateMs + profiler.sourceRopeDrawMs;
    const float profiledMs = profiler.totalProfiledMs();
    const float otherMs = std::max(0.0f, ms - profiledMs);
    float stackedMs = 0.0f;
    const ImVec2 barOrigin(x, origin.y);
    DrawStackSegment(dl, barOrigin, barWidth, kFrameGraphHeight, kFrameGraphScaleMs, &stackedMs,
                     otherMs, kFrameGraphOtherColor);
    DrawStackSegment(dl, barOrigin, barWidth, kFrameGraphHeight, kFrameGraphScaleMs, &stackedMs,
                     profiler.compositedRenderImmediateMs, kFrameGraphRenderImmediateColor);
    DrawStackSegment(dl, barOrigin, barWidth, kFrameGraphHeight, kFrameGraphScaleMs, &stackedMs,
                     profiler.compositedRenderCachedMs, kFrameGraphRenderCachedColor);
    DrawStackSegment(dl, barOrigin, barWidth, kFrameGraphHeight, kFrameGraphScaleMs, &stackedMs,
                     profiler.overlayCaptureMs, kFrameGraphOverlayCaptureColor);
    DrawStackSegment(dl, barOrigin, barWidth, kFrameGraphHeight, kFrameGraphScaleMs, &stackedMs,
                     profiler.overlayDrawMs, kFrameGraphOverlayRenderColor);
    DrawStackSegment(dl, barOrigin, barWidth, kFrameGraphHeight, kFrameGraphScaleMs, &stackedMs,
                     profiler.overlaySnapshotMs, kFrameGraphOverlaySnapshotColor);
    DrawStackSegment(dl, barOrigin, barWidth, kFrameGraphHeight, kFrameGraphScaleMs, &stackedMs,
                     profiler.overlayUploadMs, kFrameGraphOverlayUploadColor);
    DrawStackSegment(dl, barOrigin, barWidth, kFrameGraphHeight, kFrameGraphScaleMs, &stackedMs,
                     profiler.compositedUploadMs, kFrameGraphCompositedUploadColor);
    DrawStackSegment(dl, barOrigin, barWidth, kFrameGraphHeight, kFrameGraphScaleMs, &stackedMs,
                     sourceRopesMs, kFrameGraphSourceRopeColor);
    const ImU32 missColor = FrameBudgetMissColor(ms);
    if (missColor != 0) {
      dl->AddRectFilled(ImVec2(x, origin.y), ImVec2(x + barWidth, origin.y + 2.0f), missColor);
    }
  }

  DrawFrameBudgetLine(dl, origin, bottomRight, kFrameBudget120Ms, kFrameGraphBudget120Color);
  DrawFrameBudgetLine(dl, origin, bottomRight, kFrameBudget60Ms, kFrameGraphBudget60Color);

  // Async worker/presentation time overlay. Only non-zero samples are plotted —
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
      // Isolated sample — draw a small dot so it's visible.
      dl->AddRectFilled(ImVec2(point.x - 0.5f, point.y - 0.5f),
                        ImVec2(point.x + 1.0f, point.y + 1.0f), kBackendColor);
    }
    prev = point;
    havePrev = true;
  }

  ImGui::Dummy(ImVec2(kFrameGraphWidth, kFrameGraphHeight));
  const ImU32 textColor = FrameReadoutColor(displayedMs);
  ImGui::PushStyleColor(ImGuiCol_Text, textColor);
  ImGui::Text("%.2f ms / %.1f FPS", displayedMs, displayedFps);
  ImGui::PopStyleColor();
  if (displayedBackendMs > 0.0f) {
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, kBackendColor);
    ImGui::Text("  worker %.2f ms", displayedBackendMs);
    ImGui::PopStyleColor();
  }
  DrawProfilerLegendItem(kFrameGraphOtherColor, "other", /*sameLine=*/false);
  DrawProfilerLegendItem(kFrameGraphRenderImmediateColor, "rnd-imm", /*sameLine=*/true);
  DrawProfilerLegendItem(kFrameGraphRenderCachedColor, "rnd-cache", /*sameLine=*/true);
  DrawProfilerLegendItem(kFrameGraphOverlayCaptureColor, "capture", /*sameLine=*/false);
  DrawProfilerLegendItem(kFrameGraphOverlayRenderColor, "draw", /*sameLine=*/true);
  DrawProfilerLegendItem(kFrameGraphOverlaySnapshotColor, "overlay", /*sameLine=*/false);
  DrawProfilerLegendItem(kFrameGraphOverlayUploadColor, "upload", /*sameLine=*/true);
  DrawProfilerLegendItem(kFrameGraphCompositedUploadColor, "tiles", /*sameLine=*/true);
  DrawProfilerLegendItem(kFrameGraphSourceRopeColor, "ropes", /*sameLine=*/true);
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
    DrawMemoryStackSegment(dl, barOrigin, barWidth, kMemoryGraphHeight, scaleBytes, &stackedBytes,
                           memory.activeTileBytes, kMemoryActiveTileColor);
    DrawMemoryStackSegment(dl, barOrigin, barWidth, kMemoryGraphHeight, scaleBytes, &stackedBytes,
                           memory.overviewTileBytes, kMemoryOverviewTileColor);
    DrawMemoryStackSegment(dl, barOrigin, barWidth, kMemoryGraphHeight, scaleBytes, &stackedBytes,
                           memory.retiredBytes, kMemoryRetiredColor);
    DrawMemoryStackSegment(dl, barOrigin, barWidth, kMemoryGraphHeight, scaleBytes, &stackedBytes,
                           memory.overlayBytes, kMemoryOverlayColor);
    const std::uint64_t knownBytes = memory.activeTileBytes + memory.overviewTileBytes +
                                     memory.retiredBytes + memory.overlayBytes;
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
  DrawProfilerLegendItem(kMemoryActiveTileColor, "active", /*sameLine=*/false);
  DrawProfilerLegendItem(kMemoryOverviewTileColor, "overview", /*sameLine=*/true);
  DrawProfilerLegendItem(kMemoryRetiredColor, "retired", /*sameLine=*/true);
  DrawProfilerLegendItem(kMemoryOverlayColor, "overlay", /*sameLine=*/true);
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

ImVec2 ToImVec2(const Vector2d& value) {
  return ImVec2(static_cast<float>(value.x), static_cast<float>(value.y));
}

bool IsFinite(const Vector2d& value) {
  return std::isfinite(value.x) && std::isfinite(value.y);
}

double MillisecondsSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
      .count();
}

Box2d PresentedTileQuadBounds(const PresentedTileQuad& tileQuad) {
  Box2d bounds = Box2d::CreateEmpty(tileQuad.topLeft);
  bounds.addPoint(tileQuad.topRight);
  bounds.addPoint(tileQuad.bottomRight);
  bounds.addPoint(tileQuad.bottomLeft);
  return bounds;
}

bool SelectionChromeSnapshotHasContent(const SelectionChromeSnapshot& snapshot) {
  return !snapshot.paths.empty() || !snapshot.hoverPaths.empty() || !snapshot.aabbsDoc.empty() ||
         !snapshot.hoverAabbsDoc.empty() || snapshot.marqueeDoc.has_value() ||
         snapshot.orientedBoundsDoc.has_value() || !snapshot.handleBoxesDoc.empty();
}

float OverlayStrokeWidth(double strokeWidthDoc, const ViewportState& viewport) {
  const float screenWidth =
      static_cast<float>(std::abs(strokeWidthDoc * viewport.pixelsPerDocUnit()));
  return std::max(0.5f, screenWidth);
}

std::array<ImVec2, 4> ScreenBoxCorners(const ViewportState& viewport, const Box2d& docBox) {
  return {
      ToImVec2(viewport.documentToScreen(docBox.topLeft)),
      ToImVec2(viewport.documentToScreen(Vector2d(docBox.bottomRight.x, docBox.topLeft.y))),
      ToImVec2(viewport.documentToScreen(docBox.bottomRight)),
      ToImVec2(viewport.documentToScreen(Vector2d(docBox.topLeft.x, docBox.bottomRight.y))),
  };
}

Box2d CombinedSelectionBounds(std::span<const Box2d> boxes) {
  Box2d combined = boxes.front();
  for (const Box2d& box : boxes.subspan(1)) {
    combined.addPoint(box.topLeft);
    combined.addPoint(box.bottomRight);
  }
  return combined;
}

void StrokeDocumentPath(ImDrawList* drawList, const ViewportState& viewport, const Path& path,
                        ImU32 color, float thickness) {
  if (path.empty()) {
    return;
  }

  bool subpathActive = false;
  const auto flushSubpath = [&](ImDrawFlags flags) {
    if (!subpathActive) {
      drawList->PathClear();
      return;
    }

    drawList->PathStroke(color, flags, thickness);
    drawList->PathClear();
    subpathActive = false;
  };

  drawList->PathClear();
  path.forEach([&](Path::Verb verb, std::span<const Vector2d> points) {
    switch (verb) {
      case Path::Verb::MoveTo:
        flushSubpath(ImDrawFlags_None);
        drawList->PathLineTo(ToImVec2(viewport.documentToScreen(points[0])));
        subpathActive = true;
        break;
      case Path::Verb::LineTo:
        drawList->PathLineTo(ToImVec2(viewport.documentToScreen(points[0])));
        subpathActive = true;
        break;
      case Path::Verb::QuadTo:
        drawList->PathBezierQuadraticCurveTo(ToImVec2(viewport.documentToScreen(points[0])),
                                             ToImVec2(viewport.documentToScreen(points[1])), 8);
        subpathActive = true;
        break;
      case Path::Verb::CurveTo:
        drawList->PathBezierCubicCurveTo(ToImVec2(viewport.documentToScreen(points[0])),
                                         ToImVec2(viewport.documentToScreen(points[1])),
                                         ToImVec2(viewport.documentToScreen(points[2])), 8);
        subpathActive = true;
        break;
      case Path::Verb::ClosePath: flushSubpath(ImDrawFlags_Closed); break;
    }
  });
  flushSubpath(ImDrawFlags_None);
}

void StrokeDocumentBox(ImDrawList* drawList, const ViewportState& viewport, const Box2d& docBox,
                       ImU32 color, float thickness) {
  const std::array<ImVec2, 4> corners = ScreenBoxCorners(viewport, docBox);
  drawList->AddPolyline(corners.data(), static_cast<int>(corners.size()), color, ImDrawFlags_Closed,
                        thickness);
}

void FillDocumentBox(ImDrawList* drawList, const ViewportState& viewport, const Box2d& docBox,
                     ImU32 color) {
  const Box2d screenBox = viewport.documentToScreen(docBox);
  drawList->AddRectFilled(ToImVec2(screenBox.topLeft), ToImVec2(screenBox.bottomRight), color);
}

void DrawImmediateSelectionChrome(ImDrawList* drawList, const RenderPanePresenterState& state) {
  if (!state.immediateOverlaySnapshot.has_value() ||
      !SelectionChromeSnapshotHasContent(*state.immediateOverlaySnapshot)) {
    return;
  }

  const SelectionChromeSnapshot& snapshot = *state.immediateOverlaySnapshot;
  const float selectionStrokeWidth =
      OverlayStrokeWidth(snapshot.selectionStrokeWidthWorld, state.viewport);
  const float hoverStrokeWidth = OverlayStrokeWidth(snapshot.hoverStrokeWidthWorld, state.viewport);
  const float marqueeStrokeWidth =
      OverlayStrokeWidth(snapshot.marqueeStrokeWidthWorld, state.viewport);

  if (!snapshot.hoverPaths.empty()) {
    for (const SelectionChromeSnapshot::PathItem& item : snapshot.hoverPaths) {
      StrokeDocumentPath(drawList, state.viewport, item.pathDoc, kOverlaySourceHoverStrokeColor,
                         hoverStrokeWidth);
    }
  } else {
    for (const Box2d& hoverAabb : snapshot.hoverAabbsDoc) {
      StrokeDocumentBox(drawList, state.viewport, hoverAabb, kOverlaySourceHoverBoundsColor,
                        hoverStrokeWidth);
    }
  }

  for (const SelectionChromeSnapshot::PathItem& item : snapshot.paths) {
    StrokeDocumentPath(drawList, state.viewport, item.pathDoc,
                       item.displayNone ? kOverlayDisplayNoneColor : kOverlaySelectionColor,
                       selectionStrokeWidth);
  }

  if (snapshot.orientedBoundsDoc.has_value()) {
    std::array<ImVec2, 4> corners;
    std::ranges::transform(
        snapshot.orientedBoundsDoc->cornersDoc, corners.begin(),
        [&](const Vector2d& corner) { return ToImVec2(state.viewport.documentToScreen(corner)); });
    drawList->AddPolyline(corners.data(), static_cast<int>(corners.size()), kOverlaySelectionColor,
                          ImDrawFlags_Closed, selectionStrokeWidth);
  } else if (!snapshot.aabbsDoc.empty()) {
    const Box2d combinedBounds = CombinedSelectionBounds(snapshot.aabbsDoc);
    for (const Box2d& aabb : snapshot.aabbsDoc) {
      StrokeDocumentBox(drawList, state.viewport, aabb, kOverlaySelectionColor,
                        selectionStrokeWidth);
    }
    if (snapshot.aabbsDoc.size() > 1) {
      StrokeDocumentBox(drawList, state.viewport, combinedBounds, kOverlaySelectionColor,
                        selectionStrokeWidth);
    }
  }

  for (const Box2d& handleBox : snapshot.handleBoxesDoc) {
    FillDocumentBox(drawList, state.viewport, handleBox, kOverlayHandleFillColor);
    StrokeDocumentBox(drawList, state.viewport, handleBox, kOverlaySelectionColor,
                      selectionStrokeWidth);
  }

  if (snapshot.marqueeDoc.has_value()) {
    FillDocumentBox(drawList, state.viewport, *snapshot.marqueeDoc, kOverlayMarqueeFillColor);
    StrokeDocumentBox(drawList, state.viewport, *snapshot.marqueeDoc, kOverlayMarqueeStrokeColor,
                      marqueeStrokeWidth);
  }
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

  if (suppressedLayerEntity != entt::null &&
      tile.kind == RenderResult::CompositedTile::Kind::Layer &&
      tile.layerEntity == suppressedLayerEntity) {
    return false;
  }

  return true;
}

bool HasPresentableDragTargetTile(
    const GlTextureCache& textures,
    const std::optional<SelectTool::ActiveDragPreview>& activeDragPreview,
    Entity suppressedLayerEntity, bool suppressDragTargetTiles) {
  if (!activeDragPreview.has_value()) {
    return false;
  }

  const auto matchesActiveDragTarget = [&](const GlTextureCache::TileView& tile) {
    return tile.isDragTarget && tile.layerEntity == activeDragPreview->entity &&
           ShouldPresentCompositedTile(tile, suppressedLayerEntity, suppressDragTargetTiles);
  };

  if (std::ranges::any_of(textures.tiles(), matchesActiveDragTarget)) {
    return true;
  }

  return textures.activeTilesViewportBounded() &&
         std::ranges::any_of(textures.overviewTiles(), matchesActiveDragTarget);
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

RenderPanePresenterCost RenderPanePresenter::render(const RenderPanePresenterState& state) const {
  RenderPanePresenterCost cost;
  const bool hasVisibleTiles =
      std::ranges::any_of(state.textures.tiles(), [&](const GlTextureCache::TileView& tile) {
        return ShouldPresentCompositedTile(tile, state.suppressedLayerEntity,
                                           state.suppressDragTargetTiles);
      });
  const bool drawOverviewTiles = !state.textures.overviewTiles().empty();
  const bool hasVisibleOverviewTiles =
      drawOverviewTiles &&
      std::ranges::any_of(state.textures.overviewTiles(),
                          [&](const GlTextureCache::TileView& tile) {
                            return ShouldPresentCompositedTile(tile, state.suppressedLayerEntity,
                                                               state.suppressDragTargetTiles);
                          });
  const bool hasImmediateOverlay =
      state.immediateOverlaySnapshot.has_value() &&
      SelectionChromeSnapshotHasContent(*state.immediateOverlaySnapshot);
  const bool hasTextureOverlay =
      state.textures.overlayWidth() > 0 && state.textures.overlayHeight() > 0;
  const bool hasOverlay = hasImmediateOverlay || hasTextureOverlay;
  if (!hasVisibleTiles && !hasVisibleOverviewTiles && !hasOverlay) {
    ImGui::TextUnformatted("(no rendered image)");
    return cost;
  }

  const Box2d paneRect = Box2d::FromXYWH(state.viewport.paneOrigin.x, state.viewport.paneOrigin.y,
                                         state.viewport.paneSize.x, state.viewport.paneSize.y);
  if (paneRect.bottomRight.x <= paneRect.topLeft.x ||
      paneRect.bottomRight.y <= paneRect.topLeft.y) {
    return cost;
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
  DrawCheckerboard(paneDrawList, imageOrigin, imageBottomRight);

  const double pxPerDoc = state.viewport.pixelsPerDocUnit();
  const Vector2d imageOriginScreen(imageOrigin.x, imageOrigin.y);
  const Transform2d screenFromCanvasTransform =
      Transform2d::Scale(pxPerDoc) * Transform2d::Translate(imageOriginScreen);
  const std::optional<PresentedDragBaseline> dragBaseline =
      PresentedBaselineFromSelectPreviews(state.activeDragPreview, state.displayedDragPreview);
  const auto drawTile = [&](const GlTextureCache::TileView& tile) {
    if (!ShouldPresentCompositedTile(tile, state.suppressedLayerEntity,
                                     state.suppressDragTargetTiles)) {
      return;
    }
    const std::optional<PresentedTileQuad> tileQuad = ComputePresentedTileQuad(
        PresentedGeometryFromTile(tile), screenFromCanvasTransform, dragBaseline);
    if (!tileQuad.has_value()) {
      return;
    }
    if (!imageClipRect.has_value() ||
        !PresentedTileQuadIntersectsScreenRect(*tileQuad, *imageClipRect)) {
      return;
    }
    const ImVec2 uvTopLeft(0.0f, 0.0f);
    const ImVec2 uvTopRight(static_cast<float>(tile.uvBottomRight.x), 0.0f);
    const ImVec2 uvBottomRight(static_cast<float>(tile.uvBottomRight.x),
                               static_cast<float>(tile.uvBottomRight.y));
    const ImVec2 uvBottomLeft(0.0f, static_cast<float>(tile.uvBottomRight.y));
    paneDrawList->AddImageQuad(tile.texture, ToImVec2(tileQuad->topLeft),
                               ToImVec2(tileQuad->topRight), ToImVec2(tileQuad->bottomRight),
                               ToImVec2(tileQuad->bottomLeft), uvTopLeft, uvTopRight, uvBottomRight,
                               uvBottomLeft);
  };
  if (imageClipRect.has_value()) {
    paneDrawList->PushClipRect(ToImVec2(imageClipRect->topLeft),
                               ToImVec2(imageClipRect->bottomRight),
                               /*intersect_with_current_clip_rect=*/true);
    if (drawOverviewTiles) {
      for (const auto& tile : state.textures.overviewTiles()) {
        drawTile(tile);
      }
    }
    for (const auto& tile : state.textures.tiles()) {
      drawTile(tile);
    }
    paneDrawList->PopClipRect();
  }
  paneDrawList->PopClipRect();

  if (state.showOverlay && state.drawImmediateOverlay && hasImmediateOverlay &&
      imageClipRect.has_value()) {
    const auto overlayDrawStart = std::chrono::steady_clock::now();
    paneDrawList->PushClipRect(ToImVec2(imageClipRect->topLeft),
                               ToImVec2(imageClipRect->bottomRight),
                               /*intersect_with_current_clip_rect=*/true);
    DrawImmediateSelectionChrome(paneDrawList, state);
    paneDrawList->PopClipRect();
    cost.immediateOverlayDrawMs = MillisecondsSince(overlayDrawStart);
  } else if (state.showOverlay && hasTextureOverlay && imageClipRect.has_value()) {
    if (state.textures.overlayScreenRect().has_value()) {
      const Box2d& overlayScreenRect = *state.textures.overlayScreenRect();
      paneDrawList->PushClipRect(ToImVec2(imageClipRect->topLeft),
                                 ToImVec2(imageClipRect->bottomRight),
                                 /*intersect_with_current_clip_rect=*/true);
      paneDrawList->AddImage(state.textures.overlayTexture(), ToImVec2(overlayScreenRect.topLeft),
                             ToImVec2(overlayScreenRect.bottomRight), ImVec2(0.0f, 0.0f),
                             ToImVec2(state.textures.overlayUvBottomRight()));
      paneDrawList->PopClipRect();
    } else {
      const Vector2d docTopLeft = state.viewport.documentViewBox.topLeft;
      const Vector2d docTopRight(state.viewport.documentViewBox.bottomRight.x,
                                 state.viewport.documentViewBox.topLeft.y);
      const Vector2d docBottomRight = state.viewport.documentViewBox.bottomRight;
      const Vector2d docBottomLeft(state.viewport.documentViewBox.topLeft.x,
                                   state.viewport.documentViewBox.bottomRight.y);
      const auto overlayPoint = [&](const Vector2d& documentPoint) {
        return state.viewport.documentToScreen(documentPoint);
      };
      paneDrawList->PushClipRect(ToImVec2(imageClipRect->topLeft),
                                 ToImVec2(imageClipRect->bottomRight),
                                 /*intersect_with_current_clip_rect=*/true);
      const Vector2d overlayUvBottomRight = state.textures.overlayUvBottomRight();
      paneDrawList->AddImageQuad(
          state.textures.overlayTexture(), ToImVec2(overlayPoint(docTopLeft)),
          ToImVec2(overlayPoint(docTopRight)), ToImVec2(overlayPoint(docBottomRight)),
          ToImVec2(overlayPoint(docBottomLeft)), ImVec2(0.0f, 0.0f),
          ImVec2(static_cast<float>(overlayUvBottomRight.x), 0.0f), ToImVec2(overlayUvBottomRight),
          ImVec2(0.0f, static_cast<float>(overlayUvBottomRight.y)));
      paneDrawList->PopClipRect();
    }
  }

  if (state.showFrameGraph) {
    constexpr float kFramePadding = 8.0f;
    const float graphHeight = kFrameGraphHeight + kMemoryGraphHeight +
                              6.0f * ImGui::GetTextLineHeightWithSpacing() + 4.0f;
    ImGui::SetCursorPos(ImVec2(
        kFramePadding, static_cast<float>(state.contentRegion.y - graphHeight - kFramePadding)));
    RenderFrameGraph(state.frameHistory);
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    RenderMemoryGraph(state.frameHistory);
  }

  return cost;
}

}  // namespace donner::editor
