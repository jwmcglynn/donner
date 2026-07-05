#pragma once
/// @file
///
/// `EditorCommand` is the discriminated union of every UI-thread→DOM mutation
/// in the M2 scope of the editor. New tools (path, node-edit, etc.) extend
/// this variant in their own follow-up milestones - one new case per logical
/// operation, NOT one per ECS write.
///
/// All editor-side DOM writes flow through `EditorApp::applyMutation()` which
/// builds an `EditorCommand` and pushes it onto the per-frame
/// `CommandQueue`. The queue drains and coalesces at frame boundaries; see
/// `CommandQueue.h` and the "AsyncSVGDocument: single-threaded command queue"
/// section of `docs/design_docs/0020-editor.md`.

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
/// so the editor never has to touch the registry directly - every
/// payload value comes from a public SVG-level API (`EditorApp::hitTest`,
/// tree traversal, `querySelector`) and every applied mutation goes
/// through public `SVGElement` / `SVGGraphicsElement` methods.
struct EditorCommand {
  enum class Kind : std::uint8_t {
    /// Set the element's transform attribute. Used by SelectTool drag and
    /// undo/redo replay. Coalesces by element identity at flush time -
    /// multiple SetTransform commands targeting the same element collapse
    /// into the most-recently-queued one.
    SetTransform,

    /// Replace the entire document by re-parsing the given bytes. Used by
    /// file load and by the text-editor pane's typing → full-regen path.
    /// Exclusive: drains the queue of every prior command (which would
    /// reference now-invalid elements).
    ReplaceDocument,

    /// Set a single attribute on the element. Used by the structured-
    /// editing writeback path (M3) when a tool modifies an attribute
    /// value. Coalesces by `(element, attributeName)` - successive
    /// SetAttribute commands for the same element and attribute collapse
    /// to the most-recently-queued value.
    SetAttribute,

    /// Remove a single attribute from the element (DOM-level). Used to clear a
    /// marker attribute rather than leaving a falsy value behind - e.g. unlocking
    /// a layer removes `data-donner-locked` instead of writing `="false"`. No-op
    /// if the attribute is absent. Not coalesced.
    RemoveAttribute,

    /// Insert `element` as a child of `parentElement`, optionally before
    /// `referenceElement`. Used by authoring tools that create new DOM
    /// nodes. Not coalesced.
    InsertElement,

    /// Detach the element from its parent, making it invisible to the
    /// renderer. The ECS entity itself is NOT destroyed - it stays in
    /// the registry, just orphaned. This is a "soft delete" so any
    /// in-flight references (stale selection, undo snapshots) stay
    /// valid. Not coalesced.
    DeleteElement,

    /// Replace the entire document by reparsing the post-cut bytes for a
    /// shape-clipboard Cut. Behaves like ReplaceDocument with
    /// `preserveUndoOnReparse=true`, but tags the operation so undo /
    /// telemetry can label it "Cut shapes" instead of a generic reparse.
    /// Exclusive: drains the queue of every prior command. See
    /// `donner/editor/ShapeClipboardCommands.h`.
    CutShapes,

    /// Replace the entire document by reparsing the post-paste bytes for
    /// a shape-clipboard Paste / Paste in Front. Behaves like
    /// ReplaceDocument with `preserveUndoOnReparse=true`, but tags the
    /// operation so undo / telemetry can label it "Paste shapes".
    /// Exclusive: drains the queue of every prior command. See
    /// `donner/editor/ShapeClipboardCommands.h`.
    PasteShapes,

    /// Insert a `<text>` element carrying `textContent` as a child of
    /// `parentElement`, optionally before `referenceElement`. Used by the
    /// Text authoring tool (`TextTool`). The element's text content is set
    /// from `textContent` and the element is then attached exactly like
    /// InsertElement. Not coalesced.
    InsertText,

    /// Replace the element's text content with `textContent` (the element is
    /// a `<text>` or `<tspan>`). Used by the text-property inspector's
    /// content field. Coalesces by element identity at flush time -
    /// successive SetTextContent commands targeting the same element collapse
    /// to the most-recently-queued value, like SetTransform.
    SetTextContent,
  };

  Kind kind = Kind::SetTransform;

  /// Target element for SetTransform and DeleteElement. `std::nullopt`
  /// for ReplaceDocument.
  std::optional<svg::SVGElement> element;

  /// Parent element for InsertElement.
  std::optional<svg::SVGElement> parentElement;

  /// Optional sibling for InsertElement; inserted before this element
  /// when present, otherwise appended to the parent.
  std::optional<svg::SVGElement> referenceElement;

  /// SetTransform payload. Default-constructed for ReplaceDocument /
  /// DeleteElement.
  Transform2d transform;

  /// ReplaceDocument payload. Empty for SetTransform / DeleteElement.
  /// Stored by value because the source buffer (TextEditor or file
  /// contents) may go out of scope before the queue flushes.
  std::string bytes;

  /// ReplaceDocument payload: preserve the undo timeline if this reparse
  /// is the only ReplaceDocument in the flushed batch. Used by the
  /// editor's self-initiated source writeback path, which reparses to
  /// refresh XML source offsets without representing a new user-authored
  /// document baseline.
  bool preserveUndoOnReparse = false;

  /// SetAttribute payload: the attribute name (e.g. "transform", "fill").
  std::string attributeName;

  /// SetAttribute payload: the new attribute value.
  std::string attributeValue;

  /// InsertText / SetTextContent payload: the text content to apply. Stored
  /// by value because the source string (default content or inspector
  /// buffer) may go out of scope before the queue flushes.
  std::string textContent;

  /// Builds a SetTransform command.
  static EditorCommand SetTransformCommand(svg::SVGElement element, const Transform2d& transform) {
    EditorCommand cmd;
    cmd.kind = Kind::SetTransform;
    cmd.element = std::move(element);
    cmd.transform = transform;
    return cmd;
  }

  /// Builds a ReplaceDocument command from owned bytes.
  static EditorCommand ReplaceDocumentCommand(std::string bytes,
                                              bool preserveUndoOnReparse = false) {
    EditorCommand cmd;
    cmd.kind = Kind::ReplaceDocument;
    cmd.bytes = std::move(bytes);
    cmd.preserveUndoOnReparse = preserveUndoOnReparse;
    return cmd;
  }

  /// Builds a SetAttribute command.
  static EditorCommand SetAttributeCommand(svg::SVGElement element, std::string name,
                                           std::string value) {
    EditorCommand cmd;
    cmd.kind = Kind::SetAttribute;
    cmd.element = std::move(element);
    cmd.attributeName = std::move(name);
    cmd.attributeValue = std::move(value);
    return cmd;
  }

  /// Builds a RemoveAttribute command (DOM-level attribute removal).
  static EditorCommand RemoveAttributeCommand(svg::SVGElement element, std::string name) {
    EditorCommand cmd;
    cmd.kind = Kind::RemoveAttribute;
    cmd.element = std::move(element);
    cmd.attributeName = std::move(name);
    return cmd;
  }

  /// Builds an InsertElement command.
  static EditorCommand InsertElementCommand(
      svg::SVGElement parent, svg::SVGElement element,
      std::optional<svg::SVGElement> reference = std::nullopt) {
    EditorCommand cmd;
    cmd.kind = Kind::InsertElement;
    cmd.parentElement = std::move(parent);
    cmd.element = std::move(element);
    cmd.referenceElement = std::move(reference);
    return cmd;
  }

  /// Builds a DeleteElement command.
  static EditorCommand DeleteElementCommand(svg::SVGElement element) {
    EditorCommand cmd;
    cmd.kind = Kind::DeleteElement;
    cmd.element = std::move(element);
    return cmd;
  }

  /// Builds a CutShapes command from the post-cut source bytes. The caller
  /// is responsible for recording the matching undo entry on the timeline;
  /// the command itself just swaps the document.
  static EditorCommand CutShapesCommand(std::string bytes) {
    EditorCommand cmd;
    cmd.kind = Kind::CutShapes;
    cmd.bytes = std::move(bytes);
    cmd.preserveUndoOnReparse = true;
    return cmd;
  }

  /// Builds a PasteShapes command from the post-paste source bytes.
  static EditorCommand PasteShapesCommand(std::string bytes) {
    EditorCommand cmd;
    cmd.kind = Kind::PasteShapes;
    cmd.bytes = std::move(bytes);
    cmd.preserveUndoOnReparse = true;
    return cmd;
  }

  /// Builds an InsertText command. `textElement` should be a freshly-created
  /// `<text>` element; its text content is set to `content` when applied.
  static EditorCommand InsertTextCommand(svg::SVGElement parent, svg::SVGElement textElement,
                                         std::string content,
                                         std::optional<svg::SVGElement> reference = std::nullopt) {
    EditorCommand cmd;
    cmd.kind = Kind::InsertText;
    cmd.parentElement = std::move(parent);
    cmd.element = std::move(textElement);
    cmd.textContent = std::move(content);
    cmd.referenceElement = std::move(reference);
    return cmd;
  }

  /// Builds a SetTextContent command.
  static EditorCommand SetTextContentCommand(svg::SVGElement element, std::string content) {
    EditorCommand cmd;
    cmd.kind = Kind::SetTextContent;
    cmd.element = std::move(element);
    cmd.textContent = std::move(content);
    return cmd;
  }
};

}  // namespace donner::editor
