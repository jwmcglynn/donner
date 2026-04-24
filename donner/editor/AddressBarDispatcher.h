#pragma once
/// @file
///
/// **`AddressBarDispatcher`** — glues an \ref AddressBar to an \ref SvgFetcher.
///
/// Responsibilities:
///   1. When the address bar reports a navigation, dispatch it (or, if the
///      navigation carries inline bytes from a drop/picker, short-circuit
///      straight to the loader).
///   2. Update the address bar's status chip across the fetch lifecycle
///      (`kLoading` → `kRendered` / `kFetchError`).
///   3. Push completed URIs onto the address bar's history.
///
/// Pulled out of `main.cc` so the wiring is testable with a fake fetcher
/// — `main.cc` is otherwise an ImGui/GLFW-coupled translation unit that
/// doesn't unit-test.

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "donner/editor/AddressBar.h"
#include "donner/editor/AddressBarStatus.h"
#include "donner/editor/SvgFetcher.h"

namespace donner::editor {

/// Callback invoked when the dispatcher has bytes ready to load into the
/// document. The caller feeds these into `EditorBackendClient::loadBytes`
/// (or equivalent). The `resolvedPath` is the canonicalized filesystem
/// path (file loads on desktop) or empty (WASM / HTTP loads).
struct AddressBarLoadRequest {
  std::string originUri;                    ///< What the user typed / dropped.
  std::vector<uint8_t> bytes;               ///< SVG source bytes.
  std::optional<std::string> resolvedPath;  ///< For title bar + textEditor file-path display.
};

using AddressBarLoadCallback = std::function<void(const AddressBarLoadRequest&)>;

/// Orchestrates a single `AddressBar` instance on top of a single
/// `SvgFetcher`. Thread-affinity: main/UI thread only. On desktop the
/// fetcher is synchronous so every call returns before `dispatch()`
/// returns; on WASM the fetcher callback may fire on a later tick and
/// the dispatcher will transition the status chip asynchronously.
class AddressBarDispatcher {
public:
  AddressBarDispatcher(AddressBar& addressBar, SvgFetcher& fetcher, AddressBarLoadCallback onLoad);

  ~AddressBarDispatcher();

  AddressBarDispatcher(const AddressBarDispatcher&) = delete;
  AddressBarDispatcher& operator=(const AddressBarDispatcher&) = delete;

  /// Consume any pending navigation from the address bar and dispatch it.
  /// Safe to call every frame. No-op if there's nothing pending.
  void pump();

  /// Mark the currently-loaded document as successfully rendered. Called
  /// by the owner once the backend reports the document is visible (so
  /// the chip flips from `kLoading` to `kRendered`). `uri` is the origin
  /// URI from the last successful load — used only for the chip tooltip.
  void markRendered(std::string uri);

  /// Surface a parser error for the last navigation. The chip flips to
  /// `kParseError` and the message is shown as-is.
  void markParseError(std::string uri, std::string message);

private:
  void startFetch(std::string uri);
  void onFetchResult(const std::string& uri, std::optional<FetchBytes> bytes,
                     std::optional<FetchError> err);

  AddressBar& addressBar_;
  SvgFetcher& fetcher_;
  AddressBarLoadCallback onLoad_;

  std::optional<FetchHandle> activeHandle_;
};

}  // namespace donner::editor
