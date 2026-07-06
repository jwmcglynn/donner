#include "donner/editor/DialogPresenter.h"

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <utility>

#include "donner/editor/EditorTheme.h"
#include "donner/editor/ImGuiIncludes.h"

namespace donner::editor {

DialogPresenter::DialogPresenter(std::string editorNoticeText)
    : editorNoticeText_(std::move(editorNoticeText)) {}

void DialogPresenter::requestOpenFile(const std::optional<std::string>& currentFilePath) {
  std::fill(openFilePathBuffer_.begin(), openFilePathBuffer_.end(), '\0');
  if (currentFilePath.has_value()) {
    std::strncpy(openFilePathBuffer_.data(), currentFilePath->c_str(),
                 openFilePathBuffer_.size() - 1);
  }
  openFileError_.clear();
  openFileModalRequested_ = true;
}

void DialogPresenter::requestSaveFile(const std::optional<std::string>& currentFilePath,
                                      std::string error) {
  std::fill(saveFilePathBuffer_.begin(), saveFilePathBuffer_.end(), '\0');
  if (currentFilePath.has_value()) {
    std::strncpy(saveFilePathBuffer_.data(), currentFilePath->c_str(),
                 saveFilePathBuffer_.size() - 1);
  }
  saveFileError_ = std::move(error);
  saveFileModalRequested_ = true;
}

void DialogPresenter::requestAbout() {
  openAboutPopup_ = true;
}

void DialogPresenter::render(
    const std::function<bool(std::string_view, std::string*)>& tryOpenFile,
    const std::function<bool(std::string_view, std::string*)>& trySaveFile) {
  if (openAboutPopup_) {
    ImGui::OpenPopup("About Donner SVG Editor");
    openAboutPopup_ = false;
  }
  if (openFileModalRequested_) {
    ImGui::OpenPopup("Open SVG");
    openFileModalRequested_ = false;
  }
  if (saveFileModalRequested_) {
    ImGui::OpenPopup("Save SVG");
    saveFileModalRequested_ = false;
  }
  if (openLicensesPopup_) {
    ImGui::OpenPopup("Third-Party Licenses");
    openLicensesPopup_ = false;
  }

  if (ImGui::BeginPopupModal("Open SVG", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted("Open SVG");
    ImGui::Separator();
    ImGui::SetNextItemWidth(520.0f);
    const bool submitted =
        ImGui::InputText("Path", openFilePathBuffer_.data(), openFilePathBuffer_.size(),
                         ImGuiInputTextFlags_EnterReturnsTrue);
    if (!openFileError_.empty()) {
      ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(EditorTheme::Active().destructive), "%s",
                         openFileError_.c_str());
    }

    if (submitted || ImGui::Button("Open")) {
      std::string error;
      if (tryOpenFile(std::string_view(openFilePathBuffer_.data()), &error)) {
        openFileError_.clear();
        ImGui::CloseCurrentPopup();
      } else {
        openFileError_ = std::move(error);
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
      openFileError_.clear();
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  if (ImGui::BeginPopupModal("Save SVG", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted("Save SVG");
    ImGui::Separator();
    ImGui::SetNextItemWidth(520.0f);
    const bool submitted =
        ImGui::InputText("Path", saveFilePathBuffer_.data(), saveFilePathBuffer_.size(),
                         ImGuiInputTextFlags_EnterReturnsTrue);
    if (!saveFileError_.empty()) {
      ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(EditorTheme::Active().destructive), "%s",
                         saveFileError_.c_str());
    }

    if (submitted || ImGui::Button("Save")) {
      std::string error;
      if (trySaveFile(std::string_view(saveFilePathBuffer_.data()), &error)) {
        saveFileError_.clear();
        ImGui::CloseCurrentPopup();
      } else {
        saveFileError_ = std::move(error);
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
      saveFileError_.clear();
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  {
    const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(ImVec2(520.0f, 0.0f), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(displaySize.x * 0.5f, displaySize.y * 0.5f), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("About Donner SVG Editor", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::TextUnformatted("About Donner SVG Editor");
      ImGui::Separator();
      ImGui::TextUnformatted("(c) 2024-2026 Jeff McGlynn");
      ImGui::Spacing();
      ImGui::TextWrapped(
          "Part of Donner SVG Editor & Engine: an SVG editor application plus a reusable SVG "
          "rendering, geometry, and DOM engine, written in C++20.");
      ImGui::TextUnformatted("https://github.com/jwmcglynn/donner");
      ImGui::Separator();
      if (ImGui::Button("Show Licenses")) {
        openLicensesPopup_ = true;
      }
      ImGui::SameLine();
      if (ImGui::Button("Close")) {
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    }
  }

  {
    const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(ImVec2(displaySize.x * 0.75f, displaySize.y * 0.8f),
                             ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(displaySize.x * 0.5f, displaySize.y * 0.5f), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Third-Party Licenses", nullptr, ImGuiWindowFlags_NoCollapse)) {
      ImGui::TextUnformatted("Third-party license notices");
      ImGui::Separator();
      ImGui::InputTextMultiline(
          "##third_party_licenses", editorNoticeText_.data(), editorNoticeText_.size() + 1,
          ImVec2(-FLT_MIN, -ImGui::GetFrameHeightWithSpacing()), ImGuiInputTextFlags_ReadOnly);
      if (ImGui::Button("Close")) {
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    }
  }
}

void DialogPresenter::setOpenFileError(std::string error) {
  openFileError_ = std::move(error);
}

void DialogPresenter::clearOpenFileError() {
  openFileError_.clear();
}

void DialogPresenter::setSaveFileError(std::string error) {
  saveFileError_ = std::move(error);
}

void DialogPresenter::clearSaveFileError() {
  saveFileError_.clear();
}

}  // namespace donner::editor
