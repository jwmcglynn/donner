#pragma once
/// @file
///
/// `EditorCommand` is the discriminated union of every UI-thread→DOM mutation
/// in the M2 scope of the editor. New tools (path, node-edit, etc.) extend
/// this variant in their own follow-up milestones — one new case per logical
/// operation, NOT one per ECS write.
///
/// All editor-side DOM writes flow through `EditorApp::applyMutation()` which
/// builds an `EditorCommand` and pushes it onto the per-frame
/// `CommandQueue`. The queue drains and coalesces at frame boundaries; see
/// `CommandQueue.h` and the "AsyncSVGDocument: single-threaded command queue"
/// section of `docs/design_docs/editor.md`.

#include <cstdint>
#include <string>

#include "donner/base/EcsRegistry.h"
#include "donner/base/Transform.h"

namespace donner::editor {

/// Discriminated union of every editor-initiated DOM mutation in the M2
/// scope. Coalescing rules in `CommandQueue` are keyed off `kind` plus the
/// command's payload.
struct EditorCommand {
  enum class Kind : std::uint8_t {
    /// Set the element's transform attribute. Used by SelectTool drag and
    /// undo/redo replay. Coalesces by entity at flush time — multiple
    /// SetTransform commands targeting the same entity collapse into the
    /// most-recently-queued one.
    SetTransform,

    /// Replace the entire document by re-parsing the given bytes. Used by
    /// file load and by the text-editor pane's typing → full-regen path.
    /// Exclusive: drains the queue of every prior command (which would
    /// reference now-invalid entities).
    ReplaceDocument,
  };

  Kind kind = Kind::SetTransform;

  /// SetTransform payload. `entity` is `entt::null` for ReplaceDocument.
  Entity entity = entt::null;

  /// SetTransform payload. Default-constructed for ReplaceDocument.
  Transform2d transform;

  /// ReplaceDocument payload. Empty for SetTransform. Stored by value because
  /// the source buffer (TextEditor or file contents) may go out of scope
  /// before the queue flushes.
  std::string bytes;

  /// Builds a SetTransform command.
  static EditorCommand SetTransformCommand(Entity entity, const Transform2d& transform) {
    EditorCommand cmd;
    cmd.kind = Kind::SetTransform;
    cmd.entity = entity;
    cmd.transform = transform;
    return cmd;
  }

  /// Builds a ReplaceDocument command from owned bytes.
  static EditorCommand ReplaceDocumentCommand(std::string bytes) {
    EditorCommand cmd;
    cmd.kind = Kind::ReplaceDocument;
    cmd.bytes = std::move(bytes);
    return cmd;
  }
};

}  // namespace donner::editor
