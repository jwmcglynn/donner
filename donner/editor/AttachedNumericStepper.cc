#include "donner/editor/AttachedNumericStepper.h"

#include <algorithm>
#include <cmath>

#include "donner/editor/EditorTheme.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/ImGuiInternalIncludes.h"

namespace donner::editor {

namespace {

/// Vertical spacing between a numeric control and its preset popup.
constexpr double kPresetPopupGap = 3.0;

/// Height the open popup needs for all of its choices, measured from the contents it submitted
/// last frame, capped at \p maximumHeight. ImGui hides a popup for its first frame while it
/// measures, so the first visible frame already has an exact measurement.
double PreferredPopupHeight(ImGuiID popupId, float maximumHeight) {
  for (const ImGuiPopupData& popup : ImGui::GetCurrentContext()->OpenPopupStack) {
    if (popup.PopupId == popupId && popup.Window != nullptr) {
      return std::min(static_cast<double>(ImGui::CalcWindowNextAutoFitSize(popup.Window).y),
                      static_cast<double>(maximumHeight));
    }
  }
  return maximumHeight;
}

}  // namespace

AttachedNumericStepperResult RenderAttachedNumericStepper(const char* id, const ImVec2& fieldMin,
                                                          const ImVec2& fieldMax,
                                                          const EditorTheme& theme) {
  const float upperHeight = std::floor((fieldMax.y - fieldMin.y) * 0.5f);
  const float lowerHeight = fieldMax.y - fieldMin.y - upperHeight;
  const bool fieldActive = ImGui::IsItemActive();
  const ImU32 frameColor =
      ImGui::GetColorU32(fieldActive ? ImGuiCol_FrameBgActive : ImGuiCol_FrameBg);

  ImGui::SameLine(0.0f, 0.0f);
  ImGui::BeginGroup();
  ImGui::PushID(id);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
  AttachedNumericStepperResult result;
  result.increment =
      ImGui::InvisibleButton("##up", ImVec2(kAttachedNumericStepperWidth, upperHeight));
  const ImVec2 upMin = ImGui::GetItemRectMin();
  const ImVec2 upMax = ImGui::GetItemRectMax();
  const bool upHovered = ImGui::IsItemHovered();
  result.incrementRect = Box2d(Vector2d(upMin.x, upMin.y), Vector2d(upMax.x, upMax.y));

  result.decrement =
      ImGui::InvisibleButton("##down", ImVec2(kAttachedNumericStepperWidth, lowerHeight));
  const ImVec2 downMin = ImGui::GetItemRectMin();
  const ImVec2 downMax = ImGui::GetItemRectMax();
  const bool downHovered = ImGui::IsItemHovered();
  result.decrementRect = Box2d(Vector2d(downMin.x, downMin.y), Vector2d(downMax.x, downMax.y));
  ImGui::PopStyleVar();
  ImGui::PopID();
  ImGui::EndGroup();

  ImDrawList* draw = ImGui::GetWindowDrawList();
  const float radius = theme.radiusControl;
  // Cover the input's internal right-hand rounding so the pair reads as one control.
  draw->AddRectFilled(ImVec2(fieldMax.x - radius, fieldMin.y), fieldMax, frameColor);
  draw->AddRectFilled(upMin, upMax, upHovered ? theme.surfaceHover : theme.surfaceRaised, radius,
                      ImDrawFlags_RoundCornersTopRight);
  draw->AddRectFilled(downMin, downMax, downHovered ? theme.surfaceHover : theme.surfaceRaised,
                      radius, ImDrawFlags_RoundCornersBottomRight);
  draw->AddLine(ImVec2(upMin.x, fieldMin.y + 1.0f), ImVec2(upMin.x, fieldMax.y - 1.0f),
                theme.borderSubtle);
  draw->AddLine(ImVec2(upMin.x + 1.0f, upMax.y), ImVec2(upMax.x - 1.0f, upMax.y),
                theme.borderSubtle);
  draw->AddRect(fieldMin, downMax, theme.borderSubtle, radius);

  const ImU32 ink = theme.textPrimary;
  const float centerX = std::floor((upMin.x + upMax.x) * 0.5f);
  const float upY = std::floor((upMin.y + upMax.y) * 0.5f);
  const float downY = std::floor((downMin.y + downMax.y) * 0.5f);
  draw->AddTriangleFilled(ImVec2(centerX, upY - 2.0f), ImVec2(centerX - 3.0f, upY + 2.0f),
                          ImVec2(centerX + 3.0f, upY + 2.0f), ink);
  draw->AddTriangleFilled(ImVec2(centerX, downY + 2.0f), ImVec2(centerX - 3.0f, downY - 2.0f),
                          ImVec2(centerX + 3.0f, downY - 2.0f), ink);
  return result;
}

NumericPresetPopupPlacement PlaceNumericPresetPopup(const Box2d& control, double preferredHeight,
                                                    double width, const Box2d& bounds, double gap) {
  const double spaceBelow = std::max(0.0, bounds.bottomRight.y - (control.bottomRight.y + gap));
  const double spaceAbove = std::max(0.0, (control.topLeft.y - gap) - bounds.topLeft.y);
  const bool opensAbove =
      preferredHeight > spaceBelow && (preferredHeight <= spaceAbove || spaceAbove > spaceBelow);
  const double available = opensAbove ? spaceAbove : spaceBelow;
  const double x =
      std::max(bounds.topLeft.x, std::min(control.topLeft.x, bounds.bottomRight.x - width));
  return NumericPresetPopupPlacement{
      .anchor = Vector2d(x, opensAbove ? control.topLeft.y - gap : control.bottomRight.y + gap),
      .opensAbove = opensAbove,
      .maximumHeight = std::min(preferredHeight, available),
  };
}

bool BeginHybridNumericPresetPopup(const char* popupId, const ImVec2& fieldMin,
                                   const ImVec2& fieldMax, bool fieldActivated,
                                   float maximumHeight) {
  if (fieldActivated) {
    ImGui::OpenPopup(popupId);
  }
  const ImGuiID id = ImGui::GetID(popupId);
  if (!ImGui::IsPopupOpen(id, ImGuiPopupFlags_None)) {
    return false;
  }

  const float controlWidth = fieldMax.x - fieldMin.x + kAttachedNumericStepperWidth;
  const ImGuiViewport* viewport = ImGui::GetWindowViewport();
  const ImVec2 padding = ImGui::GetStyle().DisplaySafeAreaPadding;
  const Box2d bounds(Vector2d(viewport->Pos.x + padding.x, viewport->Pos.y + padding.y),
                     Vector2d(viewport->Pos.x + viewport->Size.x - padding.x,
                              viewport->Pos.y + viewport->Size.y - padding.y));
  const Box2d control(Vector2d(fieldMin.x, fieldMin.y),
                      Vector2d(fieldMax.x + kAttachedNumericStepperWidth, fieldMax.y));
  const NumericPresetPopupPlacement placement = PlaceNumericPresetPopup(
      control, PreferredPopupHeight(id, maximumHeight), controlWidth, bounds, kPresetPopupGap);
  // ImGui enforces a minimum window height, so a popup with less room than one row would spill
  // over the field; skip drawing it until the window has room again.
  if (placement.maximumHeight < ImGui::GetFrameHeightWithSpacing()) {
    return false;
  }

  // Placing the popup every frame keeps ImGui from re-fitting it into the display over the field.
  ImGui::SetNextWindowPos(
      ImVec2(static_cast<float>(placement.anchor.x), static_cast<float>(placement.anchor.y)),
      ImGuiCond_Always, ImVec2(0.0f, placement.opensAbove ? 1.0f : 0.0f));
  ImGui::SetNextWindowSizeConstraints(
      ImVec2(controlWidth, 0.0f),
      ImVec2(controlWidth, static_cast<float>(placement.maximumHeight)));
  return ImGui::BeginPopup(popupId,
                           ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNavFocus);
}

}  // namespace donner::editor
