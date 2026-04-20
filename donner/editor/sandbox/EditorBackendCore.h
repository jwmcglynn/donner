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
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "donner/editor/backend_lib/EditorApp.h"
#include "donner/editor/sandbox/EditorApiCodec.h"
#include "donner/svg/SVGElement.h"

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

  /// Handles kSelectElement. Resolves an entity from the wire handle, applies
  /// the requested selection mode, and returns a frame.
  [[nodiscard]] FramePayload handleSelectElement(const SelectElementPayload& sel);

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
  /// Bump entity-handle generation, invalidating all outstanding wire handles.
  void bumpEntityGeneration();

  /// Walk the document tree and populate \p tree.
  void populateTreeSummary(FrameTreeSummary& tree);

  /// Get or assign a stable wire id for the given entity. Also records the
  /// SVGElement so it can be resolved later.
  uint64_t entityIdFor(entt::entity entity, const svg::SVGElement& element);

  /// Resolve a wire handle back to an SVGElement, or nullopt if stale.
  [[nodiscard]] std::optional<svg::SVGElement> resolveElement(uint64_t entityId,
                                                              uint64_t entityGeneration) const;

  EditorApp editor_;
  int viewportWidth_ = 512;
  int viewportHeight_ = 384;
  uint64_t frameIdCounter_ = 1;

  /// Entity handle bimap state.
  uint64_t entityGeneration_ = 1;
  uint64_t nextEntityId_ = 1;
  std::unordered_map<entt::entity, uint64_t> entityToId_;
  std::unordered_map<uint64_t, svg::SVGElement> idToElement_;
};

}  // namespace donner::editor::sandbox
