#pragma once
/// @file

#include <span>
#include <string_view>

#include "donner/editor/MenuBarPresenter.h"

namespace donner::editor {

/// One item in the shared native/ImGui menu declaration.
struct EditorMenuNode {
  enum class Kind { Command, Separator, Submenu };

  Kind kind = Kind::Command;
  std::string_view label;
  std::string_view shortcut;
  MenuBarCommand command = MenuBarCommand::OpenAbout;
  std::span<const EditorMenuNode> children;
};

/// One top-level menu in display order.
struct EditorMenu {
  std::string_view title;
  std::span<const EditorMenuNode> items;
};

/// Enabled and checked state computed once for both menu backends.
struct EditorMenuItemPresentation {
  bool enabled = true;
  bool checked = false;
};

/// Menu backend; native editing capture disables unsafe canvas actions.
enum class EditorMenuSurface { ImGui, Native };

/// The complete ordered menu tree, including labels, shortcuts, and commands.
[[nodiscard]] std::span<const EditorMenu> EditorMenuModel();

/// Whether native key equivalents must yield to a focused text editor.
[[nodiscard]] bool SuppressNativeMenuShortcuts(const MenuBarState& state);

/// Resolve an item's live enabled and checked state from the shell's menu state.
[[nodiscard]] EditorMenuItemPresentation PresentEditorMenuItem(
    const EditorMenuNode& item, const MenuBarState& state,
    EditorMenuSurface surface = EditorMenuSurface::ImGui);

/// Gate a queued AppKit activation against current focus and item state.
[[nodiscard]] bool NativeMenuActivationAllowed(const EditorMenuNode& item,
                                               const MenuBarState& state, bool keyEquivalent);

}  // namespace donner::editor
