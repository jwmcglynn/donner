#include "donner/editor/FillStrokeWidget.h"

#include <algorithm>
#include <utility>

#include "donner/editor/EditorTheme.h"

namespace donner::editor::internal {

namespace {

// Widget sub-layout constants, relative to the widget's top-left. Fill stays
// upper-left and Stroke lower-right; draw order alone changes the foreground.
// The right column holds the angled swap arrow above one active-role None
// button, then optional custom-paint label chips.
constexpr float kSwatchSize = 28.0f;
constexpr float kFillLeft = 3.0f;
constexpr float kFillTop = 2.0f;
constexpr float kStrokeLeft = 14.0f;
constexpr float kStrokeTop = 13.0f;

constexpr float kSwapLeft = 44.0f;
constexpr float kSwapTop = 2.0f;
constexpr float kSwapWidth = 19.0f;
constexpr float kSwapHeight = 18.0f;

constexpr float kNoneLeft = 46.0f;
constexpr float kNoneTop = 24.0f;
constexpr float kNoneSize = 19.0f;

constexpr float kChipLeft = 72.0f;
constexpr float kChipRightInset = 3.0f;
constexpr float kFillChipTop = 2.0f;
constexpr float kFillChipBottom = 20.0f;
constexpr float kStrokeChipTop = 24.0f;
constexpr float kStrokeChipBottom = 42.0f;

[[nodiscard]] bool Contains(const ImVec2& min, const ImVec2& max, const ImVec2& p) {
  return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y;
}

[[nodiscard]] ImU32 SwatchColorU32(const css::RGBA& c) {
  return IM_COL32(c.r, c.g, c.b, c.a);
}

[[nodiscard]] std::string FitChipLabel(std::string label, float maxWidth) {
  if (ImGui::CalcTextSize(label.c_str()).x <= maxWidth) {
    return label;
  }
  while (label.size() > 1u) {
    label.pop_back();
    const std::string candidate = label + "...";
    if (ImGui::CalcTextSize(candidate.c_str()).x <= maxWidth) {
      return candidate;
    }
  }
  return "...";
}

void DrawPaintChip(ImDrawList* drawList, std::string_view prefix, const ToolbarPaintSlotState& slot,
                   const ImVec2& rectMin, const ImVec2& rectMax) {
  if (!slot.isCustom) {
    return;
  }
  const bool actionable = slot.reference.has_value() && slot.reference->sourceRange.has_value();
  const EditorTheme& theme = EditorTheme::Active();
  const ImU32 fillColor =
      actionable ? WithAlpha(theme.accentDefault, 210) : WithAlpha(theme.surfaceActive, 230);
  const ImU32 borderColor = actionable ? theme.accentHover : theme.borderStrong;
  drawList->AddRectFilled(rectMin, rectMax, fillColor, theme.radiusControl);
  drawList->AddRect(rectMin, rectMax, borderColor, theme.radiusControl, 0, 1.0f);
  const std::string label =
      FitChipLabel(PaintChipLabel(prefix, slot), rectMax.x - rectMin.x - 8.0f);
  const ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
  drawList->AddText(
      ImVec2(rectMin.x + 4.0f, rectMin.y + (rectMax.y - rectMin.y - textSize.y) * 0.5f - 0.5f),
      actionable ? theme.accentInk : theme.textPrimary, label.c_str());
}

}  // namespace

FillStrokeWidgetInteractionState ResolveFillStrokeWidgetInteractionState(
    bool hasDocument, bool rendererBusy, bool canvasInteractionActive,
    bool paintSnapshotMatchesSelection) {
  return FillStrokeWidgetInteractionState{
      .canEdit = hasDocument && !rendererBusy && !canvasInteractionActive,
      .refreshPaintSnapshot =
          !rendererBusy && (!canvasInteractionActive || !paintSnapshotMatchesSelection),
  };
}

FillStrokeWidgetLayout ComputeFillStrokeWidgetLayout(const ImVec2& widgetMin,
                                                     const ImVec2& widgetMax) {
  const float x = widgetMin.x;
  const float y = widgetMin.y;
  FillStrokeWidgetLayout layout;
  layout.fillMin = ImVec2(x + kFillLeft, y + kFillTop);
  layout.fillMax = ImVec2(x + kFillLeft + kSwatchSize, y + kFillTop + kSwatchSize);
  layout.strokeMin = ImVec2(x + kStrokeLeft, y + kStrokeTop);
  layout.strokeMax = ImVec2(x + kStrokeLeft + kSwatchSize, y + kStrokeTop + kSwatchSize);
  layout.swapMin = ImVec2(x + kSwapLeft, y + kSwapTop);
  layout.swapMax = ImVec2(x + kSwapLeft + kSwapWidth, y + kSwapTop + kSwapHeight);
  layout.noneMin = ImVec2(x + kNoneLeft, y + kNoneTop);
  layout.noneMax = ImVec2(x + kNoneLeft + kNoneSize, y + kNoneTop + kNoneSize);
  layout.strokeChipMin = ImVec2(x + kChipLeft, y + kStrokeChipTop);
  layout.strokeChipMax = ImVec2(widgetMax.x - kChipRightInset, y + kStrokeChipBottom);
  layout.fillChipMin = ImVec2(x + kChipLeft, y + kFillChipTop);
  layout.fillChipMax = ImVec2(widgetMax.x - kChipRightInset, y + kFillChipBottom);
  return layout;
}

FillStrokeWidgetRegion HitTestFillStrokeWidget(const FillStrokeWidgetLayout& layout,
                                               const ImVec2& point, bool fillIsCustom,
                                               bool strokeIsCustom, bool fillIsActive) {
  // Small explicit affordances win over the larger swatches / chips they sit
  // beside so they stay clickable.
  if (Contains(layout.swapMin, layout.swapMax, point)) {
    return FillStrokeWidgetRegion::Swap;
  }
  if (Contains(layout.noneMin, layout.noneMax, point)) {
    return FillStrokeWidgetRegion::SetNone;
  }
  if (strokeIsCustom && Contains(layout.strokeChipMin, layout.strokeChipMax, point)) {
    return FillStrokeWidgetRegion::StrokeChip;
  }
  if (fillIsCustom && Contains(layout.fillChipMin, layout.fillChipMax, point)) {
    return FillStrokeWidgetRegion::FillChip;
  }
  const bool fillHit = Contains(layout.fillMin, layout.fillMax, point);
  const bool strokeHit = Contains(layout.strokeMin, layout.strokeMax, point);
  if (fillHit && strokeHit) {
    return fillIsActive ? FillStrokeWidgetRegion::FillSwatch : FillStrokeWidgetRegion::StrokeSwatch;
  }
  if (fillHit) {
    return FillStrokeWidgetRegion::FillSwatch;
  }
  if (strokeHit) {
    return FillStrokeWidgetRegion::StrokeSwatch;
  }
  return FillStrokeWidgetRegion::None;
}

std::string SvgPaintStringForSlot(const ToolbarPaintSlotState& slot) {
  if (slot.isNone) {
    return "none";
  }
  if (slot.reference.has_value()) {
    std::string paint = "url(" + slot.reference->href + ")";
    if (slot.reference->fallback) {
      const css::Color& fallback = *slot.reference->fallback;
      paint += fallback.isCurrentColor() ? " currentColor" : " " + fallback.asRGBA().toHexString();
    }
    return paint;
  }
  if (slot.isCustom) {
    return slot.customLabel.empty() ? std::string("currentColor") : slot.customLabel;
  }
  return slot.color.toHexString();
}

void SwapActivePaint(ActivePaintStyle& style) {
  std::swap(style.fill, style.stroke);
}

void DrawFillStrokeSwatch(ImDrawList* drawList, const ImVec2& min, const ImVec2& max,
                          const ToolbarPaintSlotState& state, bool fillRole, bool active) {
  constexpr float kRounding = 2.5f;
  const ImU32 color = SwatchColorU32(state.color);

  if (fillRole) {
    // Fill role: solid filled square.
    drawList->AddRectFilled(min, max, color, kRounding);
  } else {
    // Stroke role: hollow ring so the two swatches read as distinct roles even
    // when they carry the same color.
    constexpr float kRing = 4.0f;
    drawList->AddRectFilled(min, max, color, kRounding);
    const ImVec2 holeMin(min.x + kRing, min.y + kRing);
    const ImVec2 holeMax(max.x - kRing, max.y - kRing);
    drawList->AddRectFilled(holeMin, holeMax, IM_COL32(32, 34, 38, 255), kRounding * 0.5f);
    drawList->AddRect(holeMin, holeMax, EditorTheme::Active().textPrimary, kRounding * 0.5f, 0,
                      1.0f);
  }

  if (state.isCustom) {
    // Diagonal hatch marks non-solid paint (gradients, patterns, context paint).
    drawList->PushClipRect(min, max, true);
    for (float lx = min.x - (max.y - min.y); lx < max.x; lx += 5.0f) {
      drawList->AddLine(ImVec2(lx, max.y), ImVec2(lx + (max.y - min.y), min.y),
                        IM_COL32(255, 255, 255, 95), 1.0f);
    }
    drawList->PopClipRect();
  }

  // A light inner keyline and accent outer border distinguish the active role
  // while keeping custom paint and the none slash legible against the theme.
  const EditorTheme& theme = EditorTheme::Active();
  drawList->AddRect(min, max, IM_COL32(255, 255, 255, 210), kRounding, 0, 1.0f);
  drawList->AddRect(min, max,
                    active || state.isCustom
                        ? theme.accentDefault
                        : (fillRole ? IM_COL32(0, 0, 0, 210) : theme.textPrimary),
                    kRounding, 0, 1.6f);

  if (state.isNone) {
    drawList->AddLine(ImVec2(min.x + 2.0f, max.y - 2.0f), ImVec2(max.x - 2.0f, min.y + 2.0f),
                      theme.destructive, 2.2f);
  }
}

void DrawSwapAffordance(ImDrawList* drawList, const ImVec2& min, const ImVec2& max, bool enabled) {
  const ImU32 tint = enabled ? IM_COL32(215, 222, 232, 255) : IM_COL32(120, 126, 134, 255);
  const ImVec2 upperLeft(min.x + 2.0f, min.y + 5.0f);
  const ImVec2 upperRight(max.x - 2.0f, upperLeft.y);
  const ImVec2 lowerLeft(min.x + 2.0f, max.y - 5.0f);
  const ImVec2 lowerRight(max.x - 2.0f, lowerLeft.y);
  drawList->AddLine(upperLeft, upperRight, tint, 1.6f);
  drawList->AddLine(lowerLeft, lowerRight, tint, 1.6f);
  drawList->AddLine(upperLeft, ImVec2(upperLeft.x + 3.0f, upperLeft.y - 3.0f), tint, 1.6f);
  drawList->AddLine(upperLeft, ImVec2(upperLeft.x + 3.0f, upperLeft.y + 3.0f), tint, 1.6f);
  drawList->AddLine(lowerRight, ImVec2(lowerRight.x - 3.0f, lowerRight.y - 3.0f), tint, 1.6f);
  drawList->AddLine(lowerRight, ImVec2(lowerRight.x - 3.0f, lowerRight.y + 3.0f), tint, 1.6f);
}

void DrawNoneAffordance(ImDrawList* drawList, const ImVec2& min, const ImVec2& max,
                        bool alreadyNone) {
  const ImU32 border = alreadyNone ? IM_COL32(232, 236, 242, 255) : IM_COL32(150, 156, 164, 255);
  drawList->AddRectFilled(min, max, IM_COL32(238, 240, 244, 255), 1.5f);
  drawList->AddRect(min, max, border, 1.5f, 0, 1.3f);
  drawList->AddLine(ImVec2(min.x + 3.0f, max.y - 3.0f), ImVec2(max.x - 3.0f, min.y + 3.0f),
                    IM_COL32(230, 40, 40, 255), 2.2f);
}

void DrawFillStrokeWidget(ImDrawList* drawList, const FillStrokeWidgetLayout& layout,
                          const ToolbarPaintState& paintState, bool fillIsActive, bool canEdit) {
  if (fillIsActive) {
    DrawFillStrokeSwatch(drawList, layout.strokeMin, layout.strokeMax, paintState.stroke,
                         /*fillRole=*/false, /*active=*/false);
    DrawFillStrokeSwatch(drawList, layout.fillMin, layout.fillMax, paintState.fill,
                         /*fillRole=*/true, /*active=*/true);
  } else {
    DrawFillStrokeSwatch(drawList, layout.fillMin, layout.fillMax, paintState.fill,
                         /*fillRole=*/true, /*active=*/false);
    DrawFillStrokeSwatch(drawList, layout.strokeMin, layout.strokeMax, paintState.stroke,
                         /*fillRole=*/false, /*active=*/true);
  }
  DrawSwapAffordance(drawList, layout.swapMin, layout.swapMax, canEdit);
  DrawNoneAffordance(drawList, layout.noneMin, layout.noneMax,
                     fillIsActive ? paintState.fill.isNone : paintState.stroke.isNone);
  DrawPaintChip(drawList, "F", paintState.fill, layout.fillChipMin, layout.fillChipMax);
  DrawPaintChip(drawList, "S", paintState.stroke, layout.strokeChipMin, layout.strokeChipMax);
}

}  // namespace donner::editor::internal
