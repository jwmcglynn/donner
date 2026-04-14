#pragma once
/// @file
///
/// Header-only in-memory implementation of ClipboardInterface. Stores
/// a single std::string in the object — no OS / ImGui interaction.
///
/// Useful in two places:
///   * Headless unit tests for TextEditorCore (the usual consumer).
///   * Non-ImGui hosts that want the same copy/cut/paste semantics
///     without pulling in an ImGui dependency.
///
/// Lives in the main donner/editor/ directory (not tests/) so that
/// production headless contexts can link against it.

#include <string>
#include <string_view>
#include <utility>

#include "donner/editor/ClipboardInterface.h"

namespace donner::editor {

/**
 * In-memory ClipboardInterface backed by a single std::string member.
 * Copying from one InMemoryClipboard and pasting into another does
 * not share state — each instance owns its own buffer.
 */
class InMemoryClipboard final : public ClipboardInterface {
public:
  InMemoryClipboard() = default;
  ~InMemoryClipboard() override = default;

  InMemoryClipboard(const InMemoryClipboard&) = delete;
  InMemoryClipboard& operator=(const InMemoryClipboard&) = delete;
  InMemoryClipboard(InMemoryClipboard&&) = delete;
  InMemoryClipboard& operator=(InMemoryClipboard&&) = delete;

  [[nodiscard]] std::string getText() const override { return clipboard_; }

  void setText(std::string_view text) override { clipboard_.assign(text); }

  [[nodiscard]] bool hasText() const override { return !clipboard_.empty(); }

private:
  std::string clipboard_;
};

}  // namespace donner::editor
