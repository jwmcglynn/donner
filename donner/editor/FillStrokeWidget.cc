#include "donner/editor/FillStrokeWidget.h"

#include <algorithm>
#include <utility>

namespace donner::editor::internal {

namespace {

// Widget sub-layout constants, relative to the widget's top-left. The widget is
// laid out as: an overlapping swatch pair on the left (fill in front / lower
// left, stroke behind / upper right), a compact affordance column (swap over a
// pair of "set none" buttons), then the custom-paint label chips.
constexpr float kSwatchSize = 21.0f;
constexpr float kFillLeft = 3.0f;
constexpr float kFillTop = 8.0f;
constexpr float kStrokeLeft = 14.0f;
constexpr float kStrokeTop = 1.0f;

constexpr float kSwapLeft = 40.0f;
constexpr float kSwapTop = 1.0f;
constexpr float kSwapWidth = 15.0f;
constexpr float kSwapHeight = 13.0f;

constexpr float kNoneTop = 17.0f;
constexpr float kNoneBottom = 28.0f;
constexpr float kStrokeNoneLeft = 40.0f;
constexpr float kStrokeNoneRight = 47.0f;
constexpr float kFillNoneLeft = 48.0f;
constexpr float kFillNoneRight = 55.0f;

constexpr float kChipLeft = 59.0f;
constexpr float kChipRightInset = 3.0f;
constexpr float kStrokeChipTop = 1.0f;
constexpr float kStrokeChipBottom = 14.0f;
constexpr float kFillChipTop = 16.0f;
constexpr float kFillChipBottom = 29.0f;

[[nodiscard]] bool Contains(const ImVec2& min, const ImVec2& max, const ImVec2& p) {
  return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y;
}

[[nodiscard]] ImU32 SwatchColorU32(const css::RGBA& c) {
  return IM_COL32(c.r, c.g, c.b, c.a);
}

}  // namespace

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
  layout.strokeNoneMin = ImVec2(x + kStrokeNoneLeft, y + kNoneTop);
  layout.strokeNoneMax = ImVec2(x + kStrokeNoneRight, y + kNoneBottom);
  layout.fillNoneMin = ImVec2(x + kFillNoneLeft, y + kNoneTop);
  layout.fillNoneMax = ImVec2(x + kFillNoneRight, y + kNoneBottom);
  layout.strokeChipMin = ImVec2(x + kChipLeft, y + kStrokeChipTop);
  layout.strokeChipMax = ImVec2(widgetMax.x - kChipRightInset, y + kStrokeChipBottom);
  layout.fillChipMin = ImVec2(x + kChipLeft, y + kFillChipTop);
  layout.fillChipMax = ImVec2(widgetMax.x - kChipRightInset, y + kFillChipBottom);
  return layout;
}

FillStrokeWidgetRegion HitTestFillStrokeWidget(const FillStrokeWidgetLayout& layout,
                                               const ImVec2& point, bool fillIsCustom,
                                               bool strokeIsCustom) {
  // Small explicit affordances win over the larger swatches / chips they sit
  // beside so they stay clickable.
  if (Contains(layout.swapMin, layout.swapMax, point)) {
    return FillStrokeWidgetRegion::Swap;
  }
  if (Contains(layout.strokeNoneMin, layout.strokeNoneMax, point)) {
    return FillStrokeWidgetRegion::StrokeNone;
  }
  if (Contains(layout.fillNoneMin, layout.fillNoneMax, point)) {
    return FillStrokeWidgetRegion::FillNone;
  }
  if (strokeIsCustom && Contains(layout.strokeChipMin, layout.strokeChipMax, point)) {
    return FillStrokeWidgetRegion::StrokeChip;
  }
  if (fillIsCustom && Contains(layout.fillChipMin, layout.fillChipMax, point)) {
    return FillStrokeWidgetRegion::FillChip;
  }
  // Fill is drawn in front, so it owns the overlapping region.
  if (Contains(layout.fillMin, layout.fillMax, point)) {
    return FillStrokeWidgetRegion::FillSwatch;
  }
  if (Contains(layout.strokeMin, layout.strokeMax, point)) {
    return FillStrokeWidgetRegion::StrokeSwatch;
  }
  return FillStrokeWidgetRegion::None;
}

std::string SvgPaintStringForSlot(const ToolbarPaintSlotState& slot) {
  if (slot.isNone) {
    return "none";
  }
  if (slot.reference.has_value()) {
    return "url(" + slot.reference->href + ")";
  }
  if (slot.isCustom) {
    return slot.customLabel.empty() ? std::string("currentColor") : slot.customLabel;
  }
  return slot.color.toHexString();
}

void SwapActivePaint(ActivePaintStyle& style) { std::swap(style.fill, style.stroke); }

void DrawFillStrokeSwatch(ImDrawList* drawList, const ImVec2& min, const ImVec2& max,
                          const ToolbarPaintSlotState& state, bool front) {
  constexpr float kRounding = 2.5f;
  const ImU32 color = SwatchColorU32(state.color);

  if (front) {
    // Fill role: solid filled square.
    drawList->AddRectFilled(min, max, color, kRounding);
  } else {
    // Stroke role: hollow ring so the two swatches read as distinct roles even
    // when they carry the same color.
    constexpr float kRing = 4.0f;
    drawList->AddRectFilled(min, max, color, kRounding);
    drawList->AddRectFilled(ImVec2(min.x + kRing, min.y + kRing), ImVec2(max.x - kRing, max.y - kRing),
                            IM_COL32(32, 34, 38, 255), kRounding * 0.5f);
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

  // Outlines: a light inner keyline plus a role-colored outer border.
  drawList->AddRect(min, max, IM_COL32(255, 255, 255, 210), kRounding, 0, 1.0f);
  drawList->AddRect(min, max, state.isCustom ? IM_COL32(91, 189, 255, 255) : IM_COL32(0, 0, 0, 210),
                    kRounding, 0, 1.6f);

  if (state.isNone) {
    drawList->AddLine(ImVec2(min.x + 2.0f, max.y - 2.0f), ImVec2(max.x - 2.0f, min.y + 2.0f),
                      IM_COL32(230, 40, 40, 255), 2.2f);
  }
}

void DrawSwapAffordance(ImDrawList* drawList, const ImVec2& min, const ImVec2& max, bool enabled) {
  const ImU32 tint = enabled ? IM_COL32(215, 222, 232, 255) : IM_COL32(120, 126, 134, 255);
  const float w = max.x - min.x;
  const float h = max.y - min.y;
  const float cx = min.x + w * 0.5f;
  const float cy = min.y + h * 0.5f;
  // A bent double-headed arrow: horizontal shaft with a head on each end, one
  // pointing left, one pointing right, evoking "swap".
  const float shaftHalf = w * 0.34f;
  const float arm = h * 0.24f;
  const ImVec2 leftTip(cx - shaftHalf, cy);
  const ImVec2 rightTip(cx + shaftHalf, cy);
  drawList->AddLine(leftTip, rightTip, tint, 1.4f);
  // Left head.
  drawList->AddLine(leftTip, ImVec2(leftTip.x + arm, leftTip.y - arm), tint, 1.4f);
  drawList->AddLine(leftTip, ImVec2(leftTip.x + arm, leftTip.y + arm), tint, 1.4f);
  // Right head.
  drawList->AddLine(rightTip, ImVec2(rightTip.x - arm, rightTip.y - arm), tint, 1.4f);
  drawList->AddLine(rightTip, ImVec2(rightTip.x - arm, rightTip.y + arm), tint, 1.4f);
}

void DrawNoneAffordance(ImDrawList* drawList, const ImVec2& min, const ImVec2& max, bool fillVariant,
                        bool active) {
  const ImU32 border = active ? IM_COL32(232, 236, 242, 255) : IM_COL32(150, 156, 164, 255);
  if (fillVariant) {
    // Solid (fill) motif: filled white square.
    drawList->AddRectFilled(min, max, IM_COL32(238, 240, 244, 255), 1.5f);
    drawList->AddRect(min, max, border, 1.5f, 0, 1.0f);
  } else {
    // Hollow (stroke) motif: ring.
    drawList->AddRect(min, max, IM_COL32(238, 240, 244, 255), 1.5f, 0, 1.6f);
    drawList->AddRect(min, max, border, 1.5f, 0, 1.0f);
  }
  // Red "none" slash across the badge.
  drawList->AddLine(ImVec2(min.x + 1.0f, max.y - 1.0f), ImVec2(max.x - 1.0f, min.y + 1.0f),
                    IM_COL32(230, 40, 40, 255), 1.6f);
}

}  // namespace donner::editor::internal
