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
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "donner/editor/AddressBarStatus.h"

namespace donner::editor {

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

  /// True while the widget is drawing a continuously-animating indicator
  /// (the indeterminate load-progress slider below the input row). The
  /// editor's on-demand render loop reads this to decide whether the
  /// frame counts as "busy" — animated widgets must re-render every frame
  /// even without input, otherwise the animation freezes. Returns false
  /// for determinate progress (a static percentage bar) and for every
  /// terminal chip state.
  [[nodiscard]] bool isLoadingAnimationActive() const {
    return statusChip_.status == AddressBarStatus::kLoading && !loadProgress_.has_value();
  }

  /// Set the current load progress, rendered as a thin bar below the
  /// input row while the chip status is `kLoading`. `nullopt` means
  /// indeterminate (animated slider) — the server didn't announce a
  /// Content-Length. Otherwise a fraction in [0, 1].
  void setLoadProgress(std::optional<float> fraction);

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
  /// Current load fraction, or `nullopt` for indeterminate. Read each
  /// frame when rendering the progress bar; written by callers.
  std::optional<float> loadProgress_;
  std::deque<std::string> history_;
  std::optional<AddressBarNavigation> pending_;
  bool focusNextFrame_ = false;
};

}  // namespace donner::editor
