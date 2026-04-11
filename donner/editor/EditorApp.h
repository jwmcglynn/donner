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

#include "donner/base/EcsRegistry.h"
#include "donner/base/Vector2.h"
#include "donner/editor/AsyncSVGDocument.h"
#include "donner/editor/EditorCommand.h"
#include "donner/svg/DonnerController.h"
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

  /// The currently-selected entity, or `entt::null` if nothing is selected.
  [[nodiscard]] Entity selectedEntity() const { return selectedEntity_; }

  /// Whether anything is selected.
  [[nodiscard]] bool hasSelection() const { return selectedEntity_ != entt::null; }

  /// Replace the current selection with a single element. Pass `entt::null`
  /// to clear the selection.
  void setSelection(Entity entity) { selectedEntity_ = entity; }

  // ---------------------------------------------------------------------------
  // Hit testing
  // ---------------------------------------------------------------------------

  /// Find the topmost geometry element at the given document-space point,
  /// or `std::nullopt` if no element is hit. Coordinates are in the SVG
  /// canvas space (the same space as the root `<svg>` viewBox).
  [[nodiscard]] std::optional<svg::SVGGeometryElement> hitTest(const Vector2d& documentPoint);

private:
  AsyncSVGDocument document_;
  Entity selectedEntity_ = entt::null;

  // Lazily-rebuilt hit-test controller. Recreated whenever the document's
  // version counter advances past the version we built the controller for.
  std::optional<svg::DonnerController> controller_;
  std::uint64_t controllerVersion_ = 0;
};

}  // namespace donner::editor
