#pragma once
/// @file
///
/// `EditorApp` is the editor's top-level shell - the mutation-seam frontend
/// that tools and the main loop interact with. Owns the `AsyncSVGDocument`,
/// the active selection, and (eventually) the active tool dispatcher.
///
/// Per `docs/design_docs/0020-editor.md`, all editor-initiated DOM writes flow
/// through `EditorApp::applyMutation()`. Tools never call
/// `SVGElement::setTransform()` directly - they build `EditorCommand`s and
/// hand them to the editor.
///
/// This is deliberately **smaller** than the prototype's `SVGState` /
/// `EditorApp` aggregates: no path-tool wiring, no overlay document, no
/// canvas pan/zoom state (that lives at the main-loop layer where it
/// belongs). It is just enough surface for `SelectTool` to do its job.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/editor/AsyncSVGDocument.h"
#include "donner/editor/AttributeWriteback.h"
#include "donner/editor/EditorCommand.h"
#include "donner/editor/LockState.h"
#include "donner/editor/UndoTimeline.h"
#include "donner/svg/DonnerController.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/SVGGeometryElement.h"

namespace donner::editor {

/// Boolean-style path operation exposed by the editor path operations panel.
enum class PathOperationKind : std::uint8_t {
  Union,
  Intersect,
  SubtractFront,
  SubtractBack,
  Exclude,
};

/// Whether a path operation can currently be applied to the editor selection.
struct PathOperationAvailability {
  bool canApply = false;  ///< True when the operation button should be enabled.
  std::string reason;     ///< Short disabled-state reason for tooltips and tests.
};

/// Whether @p command is a geometry-changing or destructive mutation targeting
/// a locked element and must be dropped by the edit-gating path. Returns true
/// only for `SetTransform` / `DeleteElement` whose target `IsLocked` (which
/// also covers descendants of a locked group). Everything else - attribute and
/// visibility/lock toggles, inserts, text edits, document replacement - is
/// allowed, so a locked layer can still be shown/hidden, selected, and
/// unlocked.
[[nodiscard]] bool IsLockGatedCommand(const EditorCommand& command);

/// Active paint settings used by authoring tools when creating new geometry.
struct ActivePaintStyle {
  // Foreground fill defaults to a visible white (design-tool convention) so new
  // geometry on a fresh document is immediately visible rather than invisible
  // `fill:none`. See Design 0013 W7 Fill/Stroke widget redesign.
  std::string fill = "white";    ///< SVG fill attribute for new geometry.
  std::string stroke = "black";  ///< SVG stroke attribute for new geometry.
  double strokeWidth = 1.0;      ///< SVG stroke-width attribute for new geometry.
};

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
  // File I/O (M7: Save)
  // ---------------------------------------------------------------------------

  /// The file path this document was loaded from, or `std::nullopt` if it
  /// was created from scratch. Populated by the main loop via
  /// `setCurrentFilePath` after a successful `File → Open` / argv load.
  [[nodiscard]] const std::optional<std::string>& currentFilePath() const {
    return currentFilePath_;
  }

  /// Set the path associated with the current document. Called by the main
  /// loop when a file is loaded. Clears the dirty flag.
  void setCurrentFilePath(std::string path) {
    currentFilePath_ = std::move(path);
    isDirty_ = false;
  }

  /// Whether the document has unsaved changes. Set automatically on every
  /// mutation via `applyMutation`; cleared by `setCurrentFilePath` /
  /// `markClean`.
  [[nodiscard]] bool isDirty() const { return isDirty_; }

  /// Mark the document as clean (e.g. after a successful save).
  void markClean() { isDirty_ = false; }

  /// Mark the document as dirty. The main loop calls this when the user
  /// types in the source pane, since text-pane edits don't go through
  /// `applyMutation`.
  void markDirty() { isDirty_ = true; }

  /// Record the current source text as the "clean" baseline. Used after
  /// loading or saving a document so later source edits can determine
  /// whether the in-memory text has diverged from the last persisted bytes.
  void setCleanSourceText(std::string_view sourceText) {
    cleanSourceText_.assign(sourceText);
    isDirty_ = false;
  }

  /// The last source text known to be in sync with the DOM.
  [[nodiscard]] std::string_view cleanSourceText() const { return cleanSourceText_; }

  /// Reload the last clean source baseline and clear transient document state.
  [[nodiscard]] bool revertToCleanSource();

  /// Recompute the dirty flag from the current source text. This allows the
  /// editor to clear the dirty indicator when the user undoes or edits back
  /// to the last clean baseline.
  void syncDirtyFromSource(std::string_view currentSourceText) {
    isDirty_ = currentSourceText != cleanSourceText_;
  }

  // ---------------------------------------------------------------------------
  // Mutation seam
  // ---------------------------------------------------------------------------

  /// The single entry point for editor-initiated DOM writes. Tools and the
  /// text pane both flow through here. Pushes the command onto the
  /// document's command queue; nothing is applied until `flushFrame()`.
  void applyMutation(EditorCommand command) {
    // Edit-gating: locked layers (`data-donner-locked="true"` on the element or
    // an ancestor) are protected from geometry-changing edits and deletion.
    // Visibility/lock metadata toggles and selection are NOT gated, so a locked
    // layer can still be shown/hidden and unlocked. See `IsLockGatedCommand`.
    if (IsLockGatedCommand(command)) {
      return;
    }
    document_.applyMutation(std::move(command));
    isDirty_ = true;
  }

  /// Set the visibility of @p element by toggling its `display` presentation
  /// attribute: hiding writes `display="none"`, showing writes
  /// `display="inline"` (a definitively visible value, observable through the
  /// computed-style visibility check). Routes through `applyMutation` so the
  /// Layers panel eye button and context menu share one code path. Visibility
  /// toggles are intentionally NOT lock-gated.
  void setElementVisible(const svg::SVGElement& element, bool visible);

  /// Lock or unlock @p element by toggling the `data-donner-locked` marker
  /// attribute (`"true"` to lock, `"false"` to unlock). Routes through
  /// `applyMutation`. The lock toggle itself is NOT lock-gated, so a locked
  /// layer can always be unlocked.
  void setElementLocked(const svg::SVGElement& element, bool locked);

  /// Restore these selection targets after the next source-backed document replacement.
  void restoreSelectionAfterNextDocumentReplace(std::vector<AttributeWritebackTarget> targets);

  /// Drain and apply any pending mutations. Called once per frame at the
  /// start of the main loop. Returns true if any commands were applied.
  bool flushFrame();

  /**
   * Delete the current selection and record a source-level undo entry.
   *
   * @param currentSourceText The source-pane text that is in sync with the
   *   current document.
   * @return true if there was a selection to delete.
   */
  bool deleteSelectionWithUndo(std::string_view currentSourceText);

  /// Paint-order ("z-order") move direction for \ref reorderSelectedElement.
  /// SVG paints in document order - later siblings paint on top - so
  /// "forward"/"front" move the element later among its siblings.
  enum class ZOrder : std::uint8_t {
    BringToFront,  ///< Move to the last sibling (paints on top of all siblings).
    SendToBack,    ///< Move to the first sibling (paints behind all siblings).
    BringForward,  ///< Move one position later (up one in paint order).
    SendBackward,  ///< Move one position earlier (down one in paint order).
  };

  /**
   * Reorder the single selected element among its siblings (paint/z-order),
   * recording one undoable structural edit.
   *
   * This is a pure DOM move - a single `SVGDocument::insertElement` of the
   * already-attached element to a new position before a computed reference
   * sibling - and the structured-editing reflection rewrites the source from the
   * DOM change. No source-text surgery (see CLAUDE.md "DOM-Level Editing Only").
   *
   * @param direction Which way to move the element in paint order.
   * @return true if the element moved; false if there is no single selection, the
   *   selection is the document root, or it is already at the requested extreme.
   */
  bool reorderSelectedElement(ZOrder direction);

  /**
   * Move @p element so it sits immediately before @p referenceSibling among its
   * current parent's children (or to the end when @p referenceSibling is
   * `std::nullopt`), recording one undoable structural edit. This is the
   * arbitrary-position generalization of \ref reorderSelectedElement used by the
   * Layers-panel drag-to-reorder affordance.
   *
   * Like the z-order moves it is a pure DOM `SVGDocument::insertElement` and the
   * structured-editing reflection rewrites the source (no source-text surgery).
   *
   * Refuses (returns false) when @p element is locked or the document root, when
   * @p referenceSibling is not a child of the same parent (cross-parent moves are
   * unsupported here), when @p referenceSibling *is* @p element, or when the
   * element is already in the requested position.
   *
   * @param element The element to move (selection is left to the caller).
   * @param referenceSibling Insert @p element before this sibling, or append when
   *   `std::nullopt`.
   * @return true if the element moved.
   */
  bool reorderElementBeforeSibling(svg::SVGElement element,
                                   std::optional<svg::SVGElement> referenceSibling);

  /**
   * Rename the single selected element's `id` to @p newId, updating every
   * internal reference so the document keeps rendering the same - one undoable
   * structural edit.
   *
   * All work is DOM-level (per CLAUDE.md "DOM-Level Editing Only"): the element's
   * `id` and every referencing attribute value (`url(#oldId)` in fill / stroke /
   * clip-path / mask / filter / markers / inline `style`, and `href` /
   * `xlink:href="#oldId"`) are changed via `SetAttributeCommand`, and the
   * structured-editing reflection rewrites the source. No source-text surgery.
   *
   * Refuses (returns false) when there is no single selection, the element is
   * locked, @p newId is empty or already used by another element, or @p newId
   * equals the current id.
   *
   * CSS `#oldId` selectors inside `<style>` blocks are rewritten as well, so
   * renames never silently break the style cascade.
   *
   * @param newId The new element id.
   * @return true if the rename was applied.
   */
  bool renameSelectedElement(std::string_view newId);

  // ---------------------------------------------------------------------------
  // Selection
  // ---------------------------------------------------------------------------

  /// All currently-selected elements, in selection order. Empty when
  /// nothing is selected. Multi-element selections come from
  /// shift+click and marquee-drag (Milestone 4 of the editor UX
  /// design doc).
  [[nodiscard]] const std::vector<svg::SVGElement>& selectedElements() const { return selection_; }

  /// Single-element accessor for back-compat with single-select call
  /// sites (overlay chrome, source-pane highlight, drag writeback,
  /// inspector, etc.). Returns the *first* selected element, or
  /// `std::nullopt` if nothing is selected. Cached so the
  /// `const optional&` reference stays stable across calls.
  [[nodiscard]] const std::optional<svg::SVGElement>& selectedElement() const {
    return cachedFirstSelection_;
  }

  /// Whether anything is selected.
  [[nodiscard]] bool hasSelection() const { return !selection_.empty(); }

  /// Replace the current selection with a single element. Pass
  /// `std::nullopt` to clear.
  void setSelection(std::optional<svg::SVGElement> element);

  /// Replace the current selection with the given list. Use this for
  /// marquee-resolved multi-selects.
  void setSelection(std::vector<svg::SVGElement> elements);

  /// Add `element` to the current selection if it isn't already
  /// selected; remove it if it is. The natural Shift+click handler.
  void toggleInSelection(const svg::SVGElement& element);

  /// Append `element` to the current selection without disturbing
  /// existing entries. No-op if `element` is already selected.
  void addToSelection(const svg::SVGElement& element);

  /**
   * Queue an attribute write for every selected element.
   *
   * @param attrName Attribute name to set, e.g. `"fill"`.
   * @param attrValue Attribute value to write.
   * @return true if commands were queued.
   */
  bool setAttributeOnSelection(std::string_view attrName, std::string_view attrValue);

  /**
   * Merge a single CSS declaration into each selected element's `style` attribute.
   *
   * @param propertyName CSS property name, e.g. `"fill"`.
   * @param propertyValue CSS property value, e.g. `"#112233"`.
   * @return true if commands were queued.
   */
  bool setStylePropertyOnSelection(std::string_view propertyName, std::string_view propertyValue);

  /**
   * Queue a `stroke-width` style-property write for every selected element.
   *
   * @param strokeWidth Stroke width in user units. Negative values clamp to zero.
   * @return true if commands were queued.
   */
  bool setStrokeWidthOnSelection(double strokeWidth);

  /// Active paint settings used by path-authoring tools for newly-created elements.
  [[nodiscard]] const ActivePaintStyle& activePaintStyle() const { return activePaintStyle_; }

  /**
   * Set the active fill attribute for newly-created elements.
   *
   * @param fill SVG fill attribute value.
   */
  void setActiveFill(std::string_view fill) { activePaintStyle_.fill = std::string(fill); }

  /**
   * Set the active stroke attribute for newly-created elements.
   *
   * @param stroke SVG stroke attribute value.
   */
  void setActiveStroke(std::string_view stroke) { activePaintStyle_.stroke = std::string(stroke); }

  /**
   * Set the active stroke width for newly-created elements.
   *
   * @param strokeWidth Stroke width in user units. Negative values clamp to zero.
   */
  void setActiveStrokeWidth(double strokeWidth);

  /// Drop every entry from the selection. Equivalent to
  /// `setSelection(std::nullopt)` but reads better at clear sites.
  void clearSelection() { setSelection(std::nullopt); }

  /**
   * Return whether a path operation is available for the current selection.
   *
   * @param operation Operation to test.
   * @return Availability and a user-facing disabled reason.
   */
  [[nodiscard]] PathOperationAvailability pathOperationAvailability(
      PathOperationKind operation) const;

  /**
   * Queue a destructive path operation over the current selection.
   *
   * Inputs are sorted by SVG paint order before dispatching to \ref PathOps so
   * selection click order cannot change Subtract Front / Subtract Back
   * semantics. The result is rejected if the operation is over the editor's
   * complexity limits or produces geometry outside the selected inputs' union
   * bounds.
   *
   * @param operation Operation to apply.
   * @return true if commands were queued.
   */
  bool applyPathOperation(PathOperationKind operation);

  /**
   * Return whether a compound path can be unbundled into separate path elements.
   *
   * If \p target is provided, availability is checked for that element. Otherwise the current
   * single-element selection is used.
   *
   * @param target Optional explicit path element to inspect.
   * @return Availability and a user-facing disabled reason.
   */
  [[nodiscard]] PathOperationAvailability compoundPathUnbundleAvailability(
      std::optional<svg::SVGElement> target = std::nullopt) const;

  /**
   * Queue an unbundle operation for one compound path.
   *
   * The source path is split into one path per contour, including hole/counter contours such as the
   * center of a letter D. New path elements inherit the original path's non-geometry attributes
   * except `id`, and the original path is removed after the replacement paths are inserted at the
   * same paint-order position.
   *
   * @param target Optional explicit path element to unbundle. If omitted, the current
   *   single-element selection is used.
   * @return true if commands were queued.
   */
  bool unbundleCompoundPath(std::optional<svg::SVGElement> target = std::nullopt);

  // ---------------------------------------------------------------------------
  // Hit testing
  // ---------------------------------------------------------------------------

  /// Find the topmost geometry element at the given document-space point,
  /// or `std::nullopt` if no element is hit. Coordinates are in the SVG
  /// canvas space (the same space as the root `<svg>` viewBox).
  [[nodiscard]] std::optional<svg::SVGGraphicsElement> hitTest(const Vector2d& documentPoint);

  /// Find every geometry element whose painted shape intersects `documentRect`. Used by marquee
  /// selection. Returns elements in document order (root-to-leaf depth-first), so callers that care
  /// about z-order can rely on a stable sequence.
  [[nodiscard]] std::vector<svg::SVGGraphicsElement> hitTestRect(const Box2d& documentRect);

  /// Return every selectable geometry element in the document, in document order (root-to-leaf
  /// depth-first). This is the canonical "Select All" set: the same elements `hitTestRect` would
  /// return for a marquee covering the whole canvas, minus the rectangle filter. Non-geometry
  /// nodes (`<defs>`, gradients, plain containers, XML text nodes) are excluded, so it matches what
  /// marquee selection treats as selectable. Empty when there is no document.
  [[nodiscard]] std::vector<svg::SVGElement> selectableElements();

  // ---------------------------------------------------------------------------
  // Undo
  // ---------------------------------------------------------------------------

  /// Access the underlying `UndoTimeline`. Tools record begin/commit
  /// transactions on it directly; `EditorApp::undo()` below is the
  /// canonical way to *apply* undo entries because it routes them
  /// through the command queue so the mutation seam is preserved.
  [[nodiscard]] UndoTimeline& undoTimeline() { return undoTimeline_; }
  [[nodiscard]] const UndoTimeline& undoTimeline() const { return undoTimeline_; }

  /**
   * Defer a single document-source undo entry to the next `flushFrame()`.
   *
   * The "before" source is captured now (by the caller, while the document is
   * still in sync); the "after" source is captured during the next
   * `flushFrame()` once queued geometry has been applied, and one undo entry is
   * recorded only if the source actually changed. This lets a multi-step
   * authoring gesture (e.g. a whole Pen-tool session) collapse into one
   * undoable command without the tool having to flush the frame itself, so the
   * normal per-frame source-sync path stays intact.
   *
   * @param label Human-readable undo label.
   * @param anchorElement Element used to anchor the source snapshot.
   * @param beforeSource Document source captured before the gesture began.
   */
  void recordDocumentSourceUndoOnNextFlush(std::string label, svg::SVGElement anchorElement,
                                           std::string beforeSource);

  /// Whether there is an entry to undo.
  [[nodiscard]] bool canUndo() const { return undoTimeline_.canUndo(); }

  /// Whether the most recently undone entry can be redone.
  [[nodiscard]] bool canRedo() const { return undoTimeline_.canRedo(); }

  /// Undo the most recent entry. Pops the timeline's next entry and
  /// pushes the restored transform onto the command queue as a
  /// `SetTransformCommand` - the actual DOM mutation happens on the
  /// next `flushFrame()`, keeping every DOM write on the same path.
  /// No-op if there is nothing to undo.
  void undo();

  /// Redo the most recently undone entry.
  ///
  /// Like `undo()`, the restored transform is routed through the command
  /// queue so the mutation seam is preserved. No-op unless the most recent
  /// timeline action was an undo.
  void redo();

  // ---------------------------------------------------------------------------
  // Structured editing (M5)
  // ---------------------------------------------------------------------------

  /// Enable or disable the structured-editing incremental path (M5).
  /// When enabled, text edits that land inside a known attribute value
  /// dispatch to `SetAttributeCommand` instead of `ReplaceDocumentCommand`,
  /// preserving tree identity. Defaults to `true`; the flag remains as a
  /// runtime escape hatch while the structured-editing rollout settles.
  void setStructuredEditingEnabled(bool enabled) { structuredEditingEnabled_ = enabled; }

  /// Whether the structured-editing incremental path is active.
  [[nodiscard]] bool structuredEditingEnabled() const { return structuredEditingEnabled_; }

  // ---------------------------------------------------------------------------
  // Canvas → text writeback queue
  // ---------------------------------------------------------------------------

  /// Payload describing a completed DOM-side transform mutation that needs
  /// to be spliced into the source text. `target` is a stable path-based
  /// reference captured while the source was still in sync with the DOM;
  /// `transform` is the local (parent-space) transform that should appear
  /// in the element's `transform=` attribute.
  struct CompletedTransformWriteback {
    AttributeWritebackTarget target;
    Transform2d transform;
    std::optional<RcString> sourceTransformAttributeValue;
    bool restoreSourceTransformAttributeValue = false;
  };

  struct CompletedElementRemoveWriteback {
    AttributeWritebackTarget target;
  };

  /// Queue a transform writeback that `main.cc` will splice into the
  /// source on its next `applyPendingTransformWriteback()` call. SelectTool
  /// calls this when a drag completes; `undo()` / `redo()` call it so
  /// undoing a canvas drag restores both the DOM transform *and* the
  /// source text in lock-step. Multiple entries are preserved so grouped
  /// multi-selection undo/redo updates every participant.
  void enqueueTransformWriteback(CompletedTransformWriteback writeback) {
    pendingTransformWritebacks_.push_back(std::move(writeback));
  }

  /// Drain the oldest queued transform writeback, if any.
  [[nodiscard]] std::optional<CompletedTransformWriteback> consumeTransformWriteback() {
    if (pendingTransformWritebacks_.empty()) {
      return std::nullopt;
    }
    auto result = std::move(pendingTransformWritebacks_.front());
    pendingTransformWritebacks_.erase(pendingTransformWritebacks_.begin());
    return result;
  }

  /// Drain all queued transform writebacks. Called once per frame by
  /// `DocumentSyncController`.
  [[nodiscard]] std::vector<CompletedTransformWriteback> consumeTransformWritebacks() {
    auto result = std::move(pendingTransformWritebacks_);
    pendingTransformWritebacks_.clear();
    return result;
  }

  /// Queue an element-removal writeback that `main.cc` will splice into the
  /// source on its next drain.
  void enqueueElementRemoveWriteback(CompletedElementRemoveWriteback writeback) {
    pendingElementRemoveWritebacks_.push_back(std::move(writeback));
  }

  /// Drain any queued element-removal writebacks.
  [[nodiscard]] std::vector<CompletedElementRemoveWriteback> consumeElementRemoveWritebacks() {
    auto result = std::move(pendingElementRemoveWritebacks_);
    pendingElementRemoveWritebacks_.clear();
    return result;
  }

private:
  /// Refreshes `cachedFirstSelection_` after `selection_` changes so
  /// the `selectedElement()` accessor can return a stable
  /// `const optional&`. Centralized so we can't forget it on a new
  /// mutation path.
  void refreshFirstSelectionCache();

  /// Records the entry deferred by \ref recordDocumentSourceUndoOnNextFlush
  /// (when the source actually changed) and clears it. Called on every
  /// `flushFrame()`, including no-op flushes with nothing queued.
  void consumePendingDocumentSourceUndo();

  /// Shared tail of the structural-move paths (\ref reorderSelectedElement and
  /// \ref reorderElementBeforeSibling): records an undo snapshot labelled
  /// @p undoLabel and issues the DOM `InsertElementCommand` that repositions
  /// @p element before @p referenceElement within @p parent. Always returns true.
  bool applyElementMove(svg::SVGElement element, svg::SVGElement parent,
                        std::optional<svg::SVGElement> referenceElement,
                        std::string_view undoLabel);

  struct PendingDocumentSourceUndo {
    std::string label;
    UndoSnapshot before;
  };

  AsyncSVGDocument document_;
  std::vector<svg::SVGElement> selection_;
  /// Mirrors `selection_.front()` (or `std::nullopt`) so the
  /// single-element compatibility accessor can hand out a reference.
  std::optional<svg::SVGElement> cachedFirstSelection_;
  UndoTimeline undoTimeline_;

  // Lazily-rebuilt hit-test controller. Recreated whenever the document's
  // version counter advances past the version we built the controller for.
  std::optional<svg::DonnerController> controller_;
  std::uint64_t controllerVersion_ = 0;

  bool structuredEditingEnabled_ = true;

  std::vector<CompletedTransformWriteback> pendingTransformWritebacks_;
  std::vector<CompletedElementRemoveWriteback> pendingElementRemoveWritebacks_;
  std::optional<PendingDocumentSourceUndo> pendingDocumentSourceUndo_;
  std::optional<std::vector<AttributeWritebackTarget>> pendingSelectionRestoreTargets_;

  std::optional<std::string> currentFilePath_;
  std::string cleanSourceText_;
  bool isDirty_ = false;
  ActivePaintStyle activePaintStyle_;
};

}  // namespace donner::editor
