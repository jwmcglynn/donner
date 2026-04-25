#pragma once
/// @file
///
/// Status-chip types for the editor's address bar. Split out of the full
/// `AddressBar.h` so that `EditorBackendClient` — which only needs to
/// carry chip payloads through `FrameResult.statusChip` — can include
/// these types without pulling in the ImGui dependency that drags GLFW
/// (and `libX11.so.6`) onto every Linux target that links the client.
///
/// The widget itself still lives in `AddressBar.h` with its ImGui-
/// driven `draw()`.

#include <string>

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

}  // namespace donner::editor
