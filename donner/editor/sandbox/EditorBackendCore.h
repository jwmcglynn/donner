#pragma once
/// @file
///
/// Shared dispatch core for the editor backend. Used by both the standalone
/// `donner_editor_backend` child process and the in-process WASM client so
/// the two paths cannot diverge. Owns an `EditorApp`, viewport state, and
/// frame-id counter.
///
/// See docs/design_docs/0023-editor_sandbox.md §S8–S9.

#include <cstdint>
#include <string_view>
#include <vector>

#include "donner/editor/backend_lib/EditorApp.h"
#include "donner/editor/sandbox/EditorApiCodec.h"

namespace donner::editor::sandbox {

/// Reusable dispatch core that services session-protocol opcodes.
///
/// The core is deliberately transport-agnostic: it consumes decoded payload
/// structs and produces encoded response payloads. The surrounding harness
/// (standalone binary or in-process client) handles framing and I/O.
class EditorBackendCore {
public:
  EditorBackendCore();
  ~EditorBackendCore();

  EditorBackendCore(const EditorBackendCore&) = delete;
  EditorBackendCore& operator=(const EditorBackendCore&) = delete;

  // -----------------------------------------------------------------------
  // Opcode handlers — each returns a FramePayload for the current state.
  // -----------------------------------------------------------------------

  /// Renders the current editor state into a `FramePayload`.
  [[nodiscard]] FramePayload buildFramePayload();

  /// Handles kSetViewport. Updates viewport dims if valid.
  [[nodiscard]] FramePayload handleSetViewport(const SetViewportPayload& vp);

  /// Handles kLoadBytes. Loads the document from raw bytes.
  [[nodiscard]] FramePayload handleLoadBytes(const LoadBytesPayload& load);

  /// Handles kReplaceSource. Replaces the source and reparses.
  [[nodiscard]] FramePayload handleReplaceSource(const ReplaceSourcePayload& rep);

  /// Handles kApplySourcePatch. Placeholder — returns current state.
  [[nodiscard]] FramePayload handleApplySourcePatch(const ApplySourcePatchPayload& patch);

  /// Handles kPointerEvent. Performs hit testing on pointer-down.
  [[nodiscard]] FramePayload handlePointerEvent(const PointerEventPayload& ptr);

  /// Handles kKeyEvent. Placeholder — returns current state.
  [[nodiscard]] FramePayload handleKeyEvent(const KeyEventPayload& key);

  /// Handles kWheelEvent. Placeholder — returns current state.
  [[nodiscard]] FramePayload handleWheelEvent(const WheelEventPayload& wheel);

  /// Handles kSetTool. Placeholder — returns current state.
  [[nodiscard]] FramePayload handleSetTool(const SetToolPayload& tool);

  /// Handles kUndo.
  [[nodiscard]] FramePayload handleUndo();

  /// Handles kRedo.
  [[nodiscard]] FramePayload handleRedo();

  /// Handles kExport. Placeholder — returns empty export response payload.
  [[nodiscard]] ExportResponsePayload handleExport(const ExportRequestPayload& req);

  /// Direct access to the underlying EditorApp (for tests).
  [[nodiscard]] EditorApp& editor() { return editor_; }

  /// Current viewport dimensions.
  [[nodiscard]] int viewportWidth() const { return viewportWidth_; }
  [[nodiscard]] int viewportHeight() const { return viewportHeight_; }

private:
  EditorApp editor_;
  int viewportWidth_ = 512;
  int viewportHeight_ = 384;
  uint64_t frameIdCounter_ = 1;
};

}  // namespace donner::editor::sandbox
