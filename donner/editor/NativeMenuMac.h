#pragma once
/// @file

#include <deque>
#include <memory>
#include <mutex>

#include "donner/editor/MenuBarPresenter.h"

namespace donner::editor {

/// macOS menu bar built from \ref EditorMenuModel.
///
/// AppKit callbacks only enqueue semantic commands and wake GLFW. The shell drains
/// them on its UI frame before applying actions to the editor.
class NativeMenuMac {
public:
  /// Actions collected while draining the native menu command queue.
  struct DrainResult {
    MenuBarActions actions;
    /// An AppKit key equivalent fired. The shell should skip its matching GLFW
    /// shortcut path for this frame to prevent a duplicate command.
    bool hadKeyEquivalent = false;
  };

  NativeMenuMac();
  ~NativeMenuMac();
  NativeMenuMac(const NativeMenuMac&) = delete;
  NativeMenuMac& operator=(const NativeMenuMac&) = delete;

  /// Install the app's main menu. Call on the macOS UI thread after GLFW startup.
  [[nodiscard]] bool install();

  /// Refresh enabled/checkmark state from the latest shell snapshot.
  void update(const MenuBarState& state);

  /// Drain queued native activations and map them through \ref ApplyMenuBarCommand.
  [[nodiscard]] DrainResult drain(const MenuBarState& state);

  /// AppKit target callback; enqueues without touching EditorShell or the DOM.
  void enqueueFromNative(MenuBarCommand command, bool keyEquivalent);

  /// Called by NSMenuDelegate while a menu is open.
  void refreshCurrentState();

private:
  struct PendingCommand {
    MenuBarCommand command;
    bool keyEquivalent;
  };
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::mutex pendingMutex_;
  std::deque<PendingCommand> pending_;
  MenuBarState currentState_;
};

}  // namespace donner::editor
