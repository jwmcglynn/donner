#include "donner/editor/AttachedNumericStepper.h"

#include <cmath>

#include "donner/editor/EditorTheme.h"
#include "donner/editor/ImGuiIncludes.h"

namespace donner::editor {

AttachedNumericStepperResult RenderAttachedNumericStepper(const char* id, const ImVec2& fieldMin,
                                                          const ImVec2& fieldMax,
                                                          const EditorTheme& theme) {
  constexpr float kButtonWidth = 22.0f;
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
  result.increment = ImGui::InvisibleButton("##up", ImVec2(kButtonWidth, upperHeight));
  const ImVec2 upMin = ImGui::GetItemRectMin();
  const ImVec2 upMax = ImGui::GetItemRectMax();
  const bool upHovered = ImGui::IsItemHovered();
  result.incrementRect = Box2d(Vector2d(upMin.x, upMin.y), Vector2d(upMax.x, upMax.y));

  result.decrement = ImGui::InvisibleButton("##down", ImVec2(kButtonWidth, lowerHeight));
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
  const float centerX = (upMin.x + upMax.x) * 0.5f;
  const float upY = (upMin.y + upMax.y) * 0.5f;
  const float downY = (downMin.y + downMax.y) * 0.5f;
  draw->AddTriangleFilled(ImVec2(centerX, upY - 2.5f), ImVec2(centerX - 3.4f, upY + 1.5f),
                          ImVec2(centerX + 3.4f, upY + 1.5f), ink);
  draw->AddTriangleFilled(ImVec2(centerX, downY + 2.5f), ImVec2(centerX - 3.4f, downY - 1.5f),
                          ImVec2(centerX + 3.4f, downY - 1.5f), ink);
  return result;
}

}  // namespace donner::editor
