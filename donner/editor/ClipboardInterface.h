#pragma once
/// @file
///
/// Abstract clipboard interface used by the editor's text-editor
/// core (see text_editor_refactor.md). Decouples copy/cut/paste
/// from ImGui so headless unit tests can inject an in-memory
/// implementation instead of requiring an ImGui context.

#include <string>
#include <string_view>

namespace donner::editor {

/**
 * Three-method interface for clipboard access. Production uses
 * ImGuiClipboard (backed by ImGui::GetClipboardText /
 * SetClipboardText). Tests use InMemoryClipboard.
 */
class ClipboardInterface {
public:
  virtual ~ClipboardInterface() = default;

  /// Returns the current clipboard contents, or an empty string if the
  /// clipboard is empty / unavailable.
  [[nodiscard]] virtual std::string getText() const = 0;

  /// Replaces the clipboard contents with \p text.
  virtual void setText(std::string_view text) = 0;

  /// Returns true if the clipboard currently contains non-empty text.
  [[nodiscard]] virtual bool hasText() const = 0;
};

}  // namespace donner::editor
