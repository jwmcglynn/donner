#include "donner/editor/SourceDiagnosticsPanel.h"

#include <algorithm>
#include <cstdio>

#include "donner/editor/ImGuiIncludes.h"

namespace donner::editor {

SourceDiagnosticCounts CountSourceDiagnostics(std::span<const SourceDiagnostic> diagnostics) {
  SourceDiagnosticCounts counts;
  for (const SourceDiagnostic& diagnostic : diagnostics) {
    if (diagnostic.severity == DiagnosticSeverity::Error) {
      ++counts.errors;
    } else {
      ++counts.warnings;
    }
  }
  return counts;
}

const SourceDiagnostic* FindSourceDiagnostic(std::span<const SourceDiagnostic> diagnostics,
                                             std::uint64_t id) {
  const auto it =
      std::find_if(diagnostics.begin(), diagnostics.end(),
                   [id](const SourceDiagnostic& diagnostic) { return diagnostic.id == id; });
  return it == diagnostics.end() ? nullptr : &*it;
}

SourceDiagnosticsPanelAction SourceDiagnosticsPanel::render(
    std::span<const SourceDiagnostic> diagnostics, std::optional<std::uint64_t> sourceHoveredId,
    float height) {
  SourceDiagnosticsPanelAction action;
  if (diagnostics.empty() || height <= 0.0f) {
    return action;
  }

  const float scale = std::max(0.75f, ImGui::GetIO().FontGlobalScale);
  const ImU32 panelColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.075f, 0.082f, 0.094f, 1.0f));
  const ImU32 separatorColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.24f, 0.27f, 0.31f, 1.0f));
  const ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);
  const ImU32 mutedColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);
  const ImU32 errorColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.95f, 0.34f, 0.31f, 1.0f));
  const ImU32 warningColor = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.72f, 0.24f, 1.0f));

  ImGui::PushStyleColor(ImGuiCol_ChildBg, panelColor);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::BeginChild("##source_diagnostics_panel", ImVec2(0.0f, height), false,
                    ImGuiWindowFlags_NoNav);

  ImDrawList* drawList = ImGui::GetWindowDrawList();
  const ImVec2 panelMin = ImGui::GetWindowPos();
  drawList->AddLine(panelMin, ImVec2(panelMin.x + ImGui::GetWindowWidth(), panelMin.y),
                    separatorColor);

  const float headerHeight = 34.0f * scale;
  const float horizontalPadding = 12.0f * scale;
  const ImVec2 headerTextPos(panelMin.x + horizontalPadding,
                             panelMin.y + (headerHeight - ImGui::GetTextLineHeight()) * 0.5f);
  drawList->AddText(headerTextPos, textColor, "Problems");

  const SourceDiagnosticCounts counts = CountSourceDiagnostics(diagnostics);
  char countLabel[96];
  std::snprintf(countLabel, sizeof(countLabel), "%zu error%s  %zu warning%s", counts.errors,
                counts.errors == 1 ? "" : "s", counts.warnings, counts.warnings == 1 ? "" : "s");
  const ImVec2 countSize = ImGui::CalcTextSize(countLabel);
  drawList->AddText(ImVec2(panelMin.x + ImGui::GetWindowWidth() - horizontalPadding - countSize.x,
                           headerTextPos.y),
                    mutedColor, countLabel);

  ImGui::SetCursorPosY(headerHeight);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
  ImGui::BeginChild("##source_diagnostics_rows", ImVec2(0.0f, 0.0f), false,
                    ImGuiWindowFlags_AlwaysVerticalScrollbar | ImGuiWindowFlags_NoNav);

  const float rowHeight = 30.0f * scale;
  for (const SourceDiagnostic& diagnostic : diagnostics) {
    ImGui::PushID(static_cast<int>(diagnostic.id ^ (diagnostic.id >> 32u)));
    const ImVec2 rowMin = ImGui::GetCursorScreenPos();
    const float rowWidth = ImGui::GetContentRegionAvail().x;
    ImGui::InvisibleButton("##diagnostic", ImVec2(rowWidth, rowHeight));
    const bool hovered = ImGui::IsItemHovered();
    const bool selected = hovered || sourceHoveredId == diagnostic.id;
    if (hovered) {
      action.hoveredId = diagnostic.id;
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
      action.activatedId = diagnostic.id;
    }

    const ImVec2 rowMax(rowMin.x + rowWidth, rowMin.y + rowHeight);
    if (selected) {
      drawList->AddRectFilled(
          rowMin, rowMax,
          ImGui::ColorConvertFloat4ToU32(ImVec4(0.22f, 0.27f, 0.34f, hovered ? 0.72f : 0.46f)));
    }
    drawList->AddLine(ImVec2(rowMin.x + horizontalPadding, rowMax.y - 1.0f),
                      ImVec2(rowMax.x - horizontalPadding, rowMax.y - 1.0f),
                      ImGui::ColorConvertFloat4ToU32(ImVec4(0.18f, 0.20f, 0.23f, 1.0f)));

    const ImU32 severityColor =
        diagnostic.severity == DiagnosticSeverity::Error ? errorColor : warningColor;
    const float centerY = rowMin.y + rowHeight * 0.5f;
    drawList->AddCircleFilled(ImVec2(rowMin.x + horizontalPadding + 4.0f * scale, centerY),
                              3.5f * scale, severityColor);

    char location[48];
    std::snprintf(location, sizeof(location), "%zu:%zu", diagnostic.line, diagnostic.column + 1);
    const ImVec2 locationSize = ImGui::CalcTextSize(location);
    const float locationX = rowMax.x - horizontalPadding - locationSize.x;
    const float textY = rowMin.y + (rowHeight - ImGui::GetTextLineHeight()) * 0.5f;
    const ImVec4 clip(rowMin.x + horizontalPadding + 18.0f * scale, rowMin.y, locationX - 8.0f,
                      rowMax.y);
    drawList->AddText(nullptr, 0.0f, ImVec2(clip.x, textY), textColor, diagnostic.message.c_str(),
                      nullptr, 0.0f, &clip);
    drawList->AddText(ImVec2(locationX, textY), mutedColor, location);
    ImGui::PopID();
  }

  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
  return action;
}

}  // namespace donner::editor
