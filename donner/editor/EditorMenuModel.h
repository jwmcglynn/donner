#pragma once
/// @file

#include <span>
#include <string_view>

#include "donner/editor/MenuBarPresenter.h"

namespace donner::editor {

/// One item in the shared native/ImGui menu declaration.
struct EditorMenuNode {
  /// Distinguishes an actionable command, visual separator, or nested menu.
  enum class Kind { Command, Separator, Submenu };

  /// How this menu node is presented and dispatched.
  Kind kind = Kind::Command;
  /// Visible label; the referenced text must outlive the menu model.
  std::string_view label;
  /// Display-only shortcut label; dispatch is handled separately.
  std::string_view shortcut;
  /// Command dispatched for a command node.
  MenuBarCommand command = MenuBarCommand::OpenAbout;
  /// Child entries for a submenu; storage must outlive the menu model.
  std::span<const EditorMenuNode> children;
};

/// One top-level menu in display order.
struct EditorMenu {
  /// Visible top-level menu title.
  std::string_view title;
  /// Entries shown under this menu; storage must outlive the menu model.
  std::span<const EditorMenuNode> items;
};

/// Enabled and checked state computed once for both menu backends.
struct EditorMenuItemPresentation {
  /// Whether the user can activate this menu item.
  bool enabled = true;
  /// Whether the item displays a selection checkmark.
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
