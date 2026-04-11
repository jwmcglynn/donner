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
#include <optional>
#include <string>

#include "donner/base/Transform.h"
#include "donner/svg/SVGElement.h"

namespace donner::editor {

/// Discriminated union of every editor-initiated DOM mutation in the M2
/// scope. Coalescing rules in `CommandQueue` are keyed off `kind` plus the
/// command's payload.
///
/// Commands carry `svg::SVGElement` handles rather than raw ECS entities
/// so the editor never has to touch the registry directly — every
/// payload value comes from a public SVG-level API (`EditorApp::hitTest`,
/// tree traversal, `querySelector`) and every applied mutation goes
/// through public `SVGElement` / `SVGGraphicsElement` methods.
struct EditorCommand {
  enum class Kind : std::uint8_t {
    /// Set the element's transform attribute. Used by SelectTool drag and
    /// undo/redo replay. Coalesces by element identity at flush time —
    /// multiple SetTransform commands targeting the same element collapse
    /// into the most-recently-queued one.
    SetTransform,

    /// Replace the entire document by re-parsing the given bytes. Used by
    /// file load and by the text-editor pane's typing → full-regen path.
    /// Exclusive: drains the queue of every prior command (which would
    /// reference now-invalid elements).
    ReplaceDocument,

    /// Detach the element from its parent, making it invisible to the
    /// renderer. The ECS entity itself is NOT destroyed — it stays in
    /// the registry, just orphaned. This is a "soft delete" so any
    /// in-flight references (stale selection, undo snapshots) stay
    /// valid. Not coalesced.
    ///
    /// Undo for DeleteElement is **not** supported in M2: the design
    /// doc defers element create/delete snapshots to when path tools
    /// return. Users wanting to undo a delete need to edit the source
    /// pane directly.
    DeleteElement,
  };

  Kind kind = Kind::SetTransform;

  /// Target element for SetTransform and DeleteElement. `std::nullopt`
  /// for ReplaceDocument.
  std::optional<svg::SVGElement> element;

  /// SetTransform payload. Default-constructed for ReplaceDocument /
  /// DeleteElement.
  Transform2d transform;

  /// ReplaceDocument payload. Empty for SetTransform / DeleteElement.
  /// Stored by value because the source buffer (TextEditor or file
  /// contents) may go out of scope before the queue flushes.
  std::string bytes;

  /// Builds a SetTransform command.
  static EditorCommand SetTransformCommand(svg::SVGElement element,
                                           const Transform2d& transform) {
    EditorCommand cmd;
    cmd.kind = Kind::SetTransform;
    cmd.element = std::move(element);
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

  /// Builds a DeleteElement command.
  static EditorCommand DeleteElementCommand(svg::SVGElement element) {
    EditorCommand cmd;
    cmd.kind = Kind::DeleteElement;
    cmd.element = std::move(element);
    return cmd;
  }
};

}  // namespace donner::editor
