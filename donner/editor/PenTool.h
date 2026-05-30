#pragma once
/// @file
///
/// Release-quality path authoring tool. `PenTool` creates source-backed
/// `<path>` elements from document-space clicks: a plain click places a line
/// anchor (`M`/`L`), a click-drag places a smooth Bezier anchor (`C`) whose
/// outgoing handle follows the mouse while the incoming handle mirrors it
/// through the anchor, and Alt/Option during the drag breaks that symmetry to
/// author a corner with mismatched handles. Shift constrains the next anchor
/// to 0/45/90 degrees from the previous one, clicking near the first anchor
/// closes the contour with `Z`, and the whole pen session collapses into a
/// single undoable command on finalize (close, Enter/double-click commit,
/// Escape cancel, or a tool switch).

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "donner/base/Vector2.h"
#include "donner/editor/Tool.h"
#include "donner/svg/SVGPathElement.h"

namespace donner::editor {

/// Click-to-place path authoring tool.
///
/// Supports polyline creation, click-drag smooth Bezier anchors with
/// Alt/Option corner-break, shift-constrained points, continuing a selected
/// open `<path>`, closing an active path by clicking near its first point,
/// and committing an open path with Enter / double-click. Every finalized pen
/// session records exactly one undoable command.
class PenTool final : public Tool {
public:
  /// Preview segment for immediate pen-tool chrome.
  struct PreviewSegment {
    Vector2d start = Vector2d::Zero();
    Vector2d control1 = Vector2d::Zero();
    Vector2d control2 = Vector2d::Zero();
    Vector2d end = Vector2d::Zero();
    bool cubic = false;
  };

  /// Preview line connecting an anchor to one of its control handles.
  struct PreviewHandleLine {
    Vector2d start = Vector2d::Zero();
    Vector2d end = Vector2d::Zero();
  };

  /// Handle a click in document space.
  ///
  /// @param editor Editor state and mutation queue.
  /// @param documentPoint Click location in SVG document coordinates.
  /// @param modifiers Modifier-key state for the click.
  void onMouseDown(EditorApp& editor, const Vector2d& documentPoint,
                   MouseModifiers modifiers) override;

  /// Handle pointer movement. While dragging the most recently placed anchor
  /// this shapes its Bezier handles; otherwise it is a no-op.
  ///
  /// @param editor Editor state and mutation queue.
  /// @param documentPoint Pointer location in SVG document coordinates.
  /// @param buttonHeld Whether the left mouse button is down.
  void onMouseMove(EditorApp& editor, const Vector2d& documentPoint, bool buttonHeld) override;

  /// Modifier-aware pointer movement. Alt/Option breaks handle symmetry so the
  /// dragged anchor becomes a corner whose incoming handle stays put while the
  /// outgoing handle follows the mouse.
  ///
  /// @param editor Editor state and mutation queue.
  /// @param documentPoint Pointer location in SVG document coordinates.
  /// @param buttonHeld Whether the left mouse button is down.
  /// @param modifiers Modifier-key state for the drag.
  void onMouseMove(EditorApp& editor, const Vector2d& documentPoint, bool buttonHeld,
                   MouseModifiers modifiers);

  /// Handle mouse release. Commits the dragged anchor's handles if it moved.
  ///
  /// @param editor Editor state and mutation queue.
  /// @param documentPoint Release location in SVG document coordinates.
  void onMouseUp(EditorApp& editor, const Vector2d& documentPoint) override;

  /// Cancel the in-progress path. Restores the document and undo stack to the
  /// state before the pen session began — Escape / cancel never leaves a
  /// partial path behind.
  void cancel(EditorApp& editor);

  /// Cancel the in-progress path without touching the document. Used when no
  /// editor is available (e.g. tool destruction); does not roll back the DOM.
  void cancel();

  /// Commit the in-progress open path without closing it (no trailing `Z`).
  /// Triggered by Enter / double-click and by tool switches. Finalizes the
  /// path as one undoable command and clears the draft. No-op when not
  /// drafting or when the path has fewer than two anchors.
  ///
  /// @param editor Editor state and mutation queue.
  /// @return true if an open path was committed.
  bool commitOpenPath(EditorApp& editor);

  /// Whether the tool is currently appending to a path.
  [[nodiscard]] bool isDrafting() const { return activePath_.has_value(); }

  /// Whether the current mouse drag is shaping the most recently placed anchor.
  [[nodiscard]] bool isDraggingAnchor() const { return draggingAnchor_; }

  /// Current path data for tests and UI diagnostics.
  [[nodiscard]] const std::string& activePathData() const { return activePathData_; }

  /// Path preview segments for immediate render-pane chrome.
  [[nodiscard]] std::vector<PreviewSegment> previewSegments() const;

  /// Anchor positions for immediate render-pane chrome.
  [[nodiscard]] std::vector<Vector2d> previewAnchors() const;

  /// Handle guide lines for immediate render-pane chrome.
  [[nodiscard]] std::vector<PreviewHandleLine> previewHandleLines() const;

  /// First anchor of the active contour, used to draw the close-path hover
  /// marker. `std::nullopt` when not drafting.
  [[nodiscard]] std::optional<Vector2d> firstAnchor() const;

  /// Whether a click at @p documentPoint would close the active path. Drives
  /// the first-anchor hover affordance.
  ///
  /// @param documentPoint Pointer location in SVG document coordinates.
  /// @param modifiers Modifier-key state (for the viewport scale).
  [[nodiscard]] bool wouldCloseAt(const Vector2d& documentPoint, MouseModifiers modifiers) const;

private:
  struct Anchor {
    Vector2d point = Vector2d::Zero();
    std::optional<Vector2d> inHandle;
    std::optional<Vector2d> outHandle;
  };

  struct OpenPathState {
    std::vector<Anchor> anchors;
  };

  [[nodiscard]] std::optional<OpenPathState> openStateForSelectedPath(
      const EditorApp& editor) const;
  [[nodiscard]] Vector2d constrainedPoint(const Vector2d& documentPoint,
                                          const MouseModifiers& modifiers) const;
  [[nodiscard]] bool shouldCloseAt(const Vector2d& documentPoint,
                                   const MouseModifiers& modifiers) const;
  [[nodiscard]] std::string serializePathData() const;
  void startNewPath(EditorApp& editor, const Vector2d& documentPoint);
  void continueSelectedPath(EditorApp& editor, const svg::SVGPathElement& path,
                            const OpenPathState& state, const Vector2d& documentPoint,
                            const MouseModifiers& modifiers);
  void rebuildActivePathData();
  void beginDragLastAnchor();
  void updateDraggedAnchor(const Vector2d& documentPoint, bool breakSymmetry);
  void commitActivePathData(EditorApp& editor);
  void appendLine(EditorApp& editor, const Vector2d& documentPoint);
  void closePath(EditorApp& editor);

  /// Capture the document source baseline so the whole pen session can later
  /// collapse into one undoable command. Called once when a session begins.
  void beginPenSession(EditorApp& editor);

  /// Record one `DocumentSource` undo entry spanning the entire pen session,
  /// then reset the draft. `editor.flushFrame()` is invoked so the recorded
  /// "after" source includes the just-committed geometry. No-op when source
  /// tracking is unavailable or nothing changed.
  void finalize(EditorApp& editor);

  std::optional<svg::SVGPathElement> activePath_;
  std::vector<Anchor> anchors_;
  Vector2d startPoint_ = Vector2d::Zero();
  Vector2d currentPoint_ = Vector2d::Zero();
  std::string activePathData_;
  bool closed_ = false;
  bool draggingAnchor_ = false;
  bool draggedAnchorChanged_ = false;
  std::size_t draggingAnchorIndex_ = 0;
  double pixelsPerDocUnit_ = 1.0;

  /// Document source captured before the first pen mutation of this session,
  /// used as the "before" side of the single finalize undo entry.
  std::optional<std::string> sessionBeforeSource_;
  /// Anchor element for the session undo snapshot (the active path).
  std::optional<svg::SVGElement> sessionUndoAnchor_;
  /// True when this session created `activePath_` from scratch (vs. continuing
  /// a pre-existing selected path). Escape only deletes paths we created.
  bool sessionCreatedPath_ = false;
};

}  // namespace donner::editor
