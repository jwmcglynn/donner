#include "donner/editor/ImGuiClipboard.h"

#include <string>
#include <string_view>

#include "donner/editor/ImGuiIncludes.h"

namespace donner::editor {

std::string ImGuiClipboard::getText() const {
  const char* text = ImGui::GetClipboardText();
  if (text == nullptr) {
    return std::string();
  }
  return std::string(text);
}

void ImGuiClipboard::setText(std::string_view text) {
  // ImGui::SetClipboardText takes a null-terminated C string, so
  // materialize the string_view into an owning std::string first.
  const std::string owned(text);
  ImGui::SetClipboardText(owned.c_str());
}

bool ImGuiClipboard::hasText() const {
  const char* text = ImGui::GetClipboardText();
  return text != nullptr && text[0] != '\0';
}

}  // namespace donner::editor
