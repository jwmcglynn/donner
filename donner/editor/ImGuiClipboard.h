#pragma once
/// @file
///
/// Production ClipboardInterface implementation backed by ImGui's
/// clipboard functions (ImGui::GetClipboardText / SetClipboardText).
///
/// This file is the ONLY place in the editor that is permitted to
/// touch ImGui::*Clipboard* APIs — it is the isolation point that
/// lets TextEditorCore stay headless-testable. Any other consumer
/// should depend on ClipboardInterface, not on ImGui directly.

#include <string>
#include <string_view>

#include "donner/editor/ClipboardInterface.h"

namespace donner::editor {

/**
 * ClipboardInterface implementation that routes through ImGui's
 * built-in clipboard. Requires an active ImGui context; calling
 * getText/setText/hasText outside of one is a programming error.
 */
class ImGuiClipboard final : public ClipboardInterface {
public:
  ImGuiClipboard() = default;
  ~ImGuiClipboard() override = default;

  ImGuiClipboard(const ImGuiClipboard&) = delete;
  ImGuiClipboard& operator=(const ImGuiClipboard&) = delete;
  ImGuiClipboard(ImGuiClipboard&&) = delete;
  ImGuiClipboard& operator=(ImGuiClipboard&&) = delete;

  [[nodiscard]] std::string getText() const override;
  void setText(std::string_view text) override;
  [[nodiscard]] bool hasText() const override;
};

}  // namespace donner::editor
