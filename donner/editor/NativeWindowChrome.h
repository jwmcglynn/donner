#pragma once
/// @file
///
/// Native window chrome integration for the editor's title bar.
///
/// Two concerns live here:
///   1. `ComposeWindowTitle` - pure derivation of the title-bar text from
///      the open file and unsaved-changes state. Testable headlessly.
///   2. The platform hooks - on macOS these drive the native title bar's
///      edited "dot" (`setDocumentEdited:`) and proxy icon
///      (`setRepresentedURL:`), so the editor behaves like a real Mac app.
///      Other platforms provide no-op stubs.

#include <optional>
#include <string>

struct GLFWwindow;

namespace donner::editor {

/// Inputs to the window title / chrome derivation.
struct WindowChromeState {
  /// Path of the open document, or `std::nullopt` for an untitled buffer.
  std::optional<std::string> filePath;
  /// Whether the document has unsaved changes.
  bool edited = false;
};

/// Compose the window title-bar text.
///
/// @param state Current document/edited state.
/// @param showEditedDotInText When true, an unsaved document is prefixed
///   with a bullet ("● ") in the text itself. Callers pass false on
///   platforms that show the edited state natively (macOS), where the dot
///   is rendered by the OS via `setDocumentEdited:` instead.
/// @return Title such as "diagram.svg - Donner SVG Editor" or, for an
///   unsaved untitled buffer with `showEditedDotInText`, "● untitled -
///   Donner SVG Editor".
[[nodiscard]] std::string ComposeWindowTitle(const WindowChromeState& state,
                                             bool showEditedDotInText);

/// Whether this platform/build drives native title-bar chrome (the edited
/// dot and proxy icon). True only on macOS.
[[nodiscard]] bool NativeWindowChromeAvailable();

/// Push the document's edited state and file identity into the native
/// title bar.
///
/// On macOS this sets the window's `documentEdited` flag (the close-button
/// dot) and `representedURL` (the draggable proxy icon). No-op elsewhere,
/// or when \p window is null.
///
/// @param window GLFW window backing the editor.
/// @param state Current document/edited state. A missing or non-existent
///   file path clears the proxy icon.
void ApplyNativeWindowChrome(GLFWwindow* window, const WindowChromeState& state);

}  // namespace donner::editor
