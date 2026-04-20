#pragma once
/// @file
///
/// **`AddressBar`** — ImGui widget for URL / path entry in the editor. Used
/// from both the desktop shell (`donner/editor/main.cc`) and the WASM
/// build; the fetcher behind it differs but the widget surface is the
/// same. See docs/design_docs/0023-editor_sandbox.md §S10.
///
/// The widget is input/output only: it captures the user's URI, exposes
/// the resulting `Navigation` when the user presses Enter / Load / drags
/// a file, and renders the status chip for whatever state the owner sets.
/// Fetching, policy enforcement, and document replacement happen in the
/// owner (typically `EditorApp`).

#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace donner::editor {

/// Status chip shown next to the URL input. Colors match the existing
/// slim-shell conventions (green / amber / blue / red / grey).
enum class AddressBarStatus {
  kEmpty,
  kLoading,
  kRendered,
  kRenderedLossy,
  kFetchError,
  kParseError,
  kPolicyDenied,
  kSandboxCrashed,
};

struct AddressBarStatusChip {
  AddressBarStatus status = AddressBarStatus::kEmpty;
  std::string message;  ///< Human-readable detail for the tooltip.
  std::string uri;      ///< Currently-loaded URI; empty when none.
};

/// Populated payload when the user triggers a navigation. When `bytes` is
/// non-empty the UI shortcutted the fetch (e.g. file drop on WASM), and
/// the owner should skip its fetcher and feed the bytes straight to the
/// document session. Otherwise the owner dispatches via its fetcher.
struct AddressBarNavigation {
  std::string uri;
  std::vector<uint8_t> bytes;
};

class AddressBar {
public:
  AddressBar();

  /// Draw the widget. Returns true when a navigation was triggered this
  /// frame (the owner should call `consumeNavigation()` to retrieve it).
  bool draw();

  /// Retrieve and clear any pending navigation. Thread: main/UI.
  [[nodiscard]] std::optional<AddressBarNavigation> consumeNavigation();

  /// Update the status chip shown next to the URL input.
  void setStatus(AddressBarStatusChip chip);

  /// Seed the input with a URI (e.g. argv[1] on desktop launch). The
  /// caller is expected to immediately dispatch a navigation for it.
  void setInitialUri(std::string_view uri);

  /// Push a URI into the LRU history. Deduped; most-recent first.
  void pushHistory(std::string uri);

  /// Access the in-memory history (for tests / debugging).
  [[nodiscard]] const std::deque<std::string>& history() const { return history_; }

  /// Report `hasDropPayload = true` when the platform shell has a pending
  /// drag-drop payload it wants the address bar to consume next frame.
  /// Used by the GLFW drop callback / WASM drop handler to wake the
  /// widget.
  void notifyDropPayload(std::string uri, std::vector<uint8_t> bytes);

private:
  static constexpr std::size_t kInputBufferSize = 2048;
  static constexpr std::size_t kHistoryCap = 16;

  std::vector<char> inputBuffer_;
  AddressBarStatusChip statusChip_;
  std::deque<std::string> history_;
  std::optional<AddressBarNavigation> pending_;
  bool focusNextFrame_ = false;
};

}  // namespace donner::editor
