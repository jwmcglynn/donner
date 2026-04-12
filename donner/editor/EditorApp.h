#pragma once
/// @file
///
/// `EditorApp` is the editor's top-level shell — the mutation-seam frontend
/// that tools and the main loop interact with. Owns the `AsyncSVGDocument`,
/// the active selection, and (eventually) the active tool dispatcher.
///
/// Per `docs/design_docs/editor.md`, all editor-initiated DOM writes flow
/// through `EditorApp::applyMutation()`. Tools never call
/// `SVGElement::setTransform()` directly — they build `EditorCommand`s and
/// hand them to the editor.
///
/// This is deliberately **smaller** than the prototype's `SVGState` /
/// `EditorApp` aggregates: no path-tool wiring, no overlay document, no
/// canvas pan/zoom state (that lives at the main-loop layer where it
/// belongs). It is just enough surface for `SelectTool` to do its job.

#include <optional>
#include <string_view>

#include "donner/base/Vector2.h"
#include "donner/editor/AsyncSVGDocument.h"
#include "donner/editor/EditorCommand.h"
#include "donner/editor/UndoTimeline.h"
#include "donner/svg/DonnerController.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/SVGGeometryElement.h"

namespace donner::editor {

/// Top-level editor shell.
///
/// Lifetime: typically one per window. All public methods are UI-thread only.
class EditorApp {
public:
  EditorApp();
  ~EditorApp() = default;

  EditorApp(const EditorApp&) = delete;
  EditorApp& operator=(const EditorApp&) = delete;
  EditorApp(EditorApp&&) = delete;
  EditorApp& operator=(EditorApp&&) = delete;

  // ---------------------------------------------------------------------------
  // Document
  // ---------------------------------------------------------------------------

  /// Load an SVG document from a string. Replaces any current document and
  /// clears the current selection. Returns true on parse success.
  [[nodiscard]] bool loadFromString(std::string_view svgBytes);

  /// Whether a document has been loaded.
  [[nodiscard]] bool hasDocument() const { return document_.hasDocument(); }

  /// Direct access to the wrapped `AsyncSVGDocument`. Used by the main loop
  /// for `flushFrame()` and `currentFrameVersion()`, and by tests.
  [[nodiscard]] AsyncSVGDocument& document() { return document_; }
  [[nodiscard]] const AsyncSVGDocument& document() const { return document_; }

  // ---------------------------------------------------------------------------
  // Mutation seam
  // ---------------------------------------------------------------------------

  /// The single entry point for editor-initiated DOM writes. Tools and the
  /// text pane both flow through here. Pushes the command onto the
  /// document's command queue; nothing is applied until `flushFrame()`.
  void applyMutation(EditorCommand command) {
    document_.applyMutation(std::move(command));
  }

  /// Drain and apply any pending mutations. Called once per frame at the
  /// start of the main loop. Returns true if any commands were applied.
  bool flushFrame();

  // ---------------------------------------------------------------------------
  // Selection
  // ---------------------------------------------------------------------------

  /// The currently-selected element, or `std::nullopt` if nothing is
  /// selected.
  [[nodiscard]] const std::optional<svg::SVGElement>& selectedElement() const {
    return selectedElement_;
  }

  /// Whether anything is selected.
  [[nodiscard]] bool hasSelection() const { return selectedElement_.has_value(); }

  /// Replace the current selection with a single element. Pass
  /// `std::nullopt` to clear the selection.
  void setSelection(std::optional<svg::SVGElement> element) {
    selectedElement_ = std::move(element);
  }

  // ---------------------------------------------------------------------------
  // Hit testing
  // ---------------------------------------------------------------------------

  /// Find the topmost geometry element at the given document-space point,
  /// or `std::nullopt` if no element is hit. Coordinates are in the SVG
  /// canvas space (the same space as the root `<svg>` viewBox).
  [[nodiscard]] std::optional<svg::SVGGeometryElement> hitTest(const Vector2d& documentPoint);

  // ---------------------------------------------------------------------------
  // Undo
  // ---------------------------------------------------------------------------

  /// Access the underlying `UndoTimeline`. Tools record begin/commit
  /// transactions on it directly; `EditorApp::undo()` below is the
  /// canonical way to *apply* undo entries because it routes them
  /// through the command queue so the mutation seam is preserved.
  [[nodiscard]] UndoTimeline& undoTimeline() { return undoTimeline_; }
  [[nodiscard]] const UndoTimeline& undoTimeline() const { return undoTimeline_; }

  /// Whether there is an entry to undo.
  [[nodiscard]] bool canUndo() const { return undoTimeline_.canUndo(); }

  /// Undo the most recent entry. Pops the timeline's next entry and
  /// pushes the restored transform onto the command queue as a
  /// `SetTransformCommand` — the actual DOM mutation happens on the
  /// next `flushFrame()`, keeping every DOM write on the same path.
  /// No-op if there is nothing to undo.
  void undo();

  /// Redo the most recently undone entry.
  ///
  /// In the non-destructive `UndoTimeline` model, "redo" is mechanically
  /// identical to "undo the most recent undo-entry": breaking the
  /// current undo chain and then calling `undo()` again pops the
  /// undo-entry the previous `undo()` call appended, which restores
  /// the post-drag state.
  ///
  /// Like `undo()`, the restored transform is routed through the
  /// command queue so the mutation seam is preserved.
  void redo();

  // ---------------------------------------------------------------------------
  // Structured editing (M5)
  // ---------------------------------------------------------------------------

  /// Enable or disable the structured-editing incremental path (M5).
  /// When enabled, text edits that land inside a known attribute value
  /// dispatch to `SetAttributeCommand` instead of `ReplaceDocumentCommand`,
  /// preserving tree identity. Defaults to `false` — the flag is flipped
  /// after the fuzzing soak (M8 in the design doc).
  void setStructuredEditingEnabled(bool enabled) { structuredEditingEnabled_ = enabled; }

  /// Whether the structured-editing incremental path is active.
  [[nodiscard]] bool structuredEditingEnabled() const { return structuredEditingEnabled_; }

private:
  AsyncSVGDocument document_;
  std::optional<svg::SVGElement> selectedElement_;
  UndoTimeline undoTimeline_;

  // Lazily-rebuilt hit-test controller. Recreated whenever the document's
  // version counter advances past the version we built the controller for.
  std::optional<svg::DonnerController> controller_;
  std::uint64_t controllerVersion_ = 0;

  bool structuredEditingEnabled_ = false;
};

}  // namespace donner::editor
