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
/// closes the contour with `Z` (dragging on close shapes the closing
/// anchor's handles), and the whole pen session collapses into a single
/// undoable command on finalize (close, Enter/double-click or Escape
/// commit, or a tool switch).

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "donner/base/Path.h"
#include "donner/base/Vector2.h"
#include "donner/editor/Tool.h"
#include "donner/svg/SVGPathElement.h"

namespace donner::editor {

/// Click-to-place path authoring tool with direct point editing.
///
/// Supports polyline creation, click-drag smooth Bezier anchors with
/// Alt/Option corner-break, shift-constrained points (45-degree increments),
/// continuing a selected open `<path>`, closing an active path by clicking
/// near its first point, and committing an open path with Enter /
/// double-click. Every finalized pen session records exactly one undoable
/// command.
///
/// The visible path-point chrome is directly editable: anchors and control
/// points of the active draft - or of a selected, already-committed `<path>`
/// - are hit targets. Dragging a control point reshapes that handle (aligned
/// coupling on smooth anchors; Alt/Option breaks the coupling; Shift
/// constrains the handle angle to 45-degree increments), dragging an anchor
/// moves it together with its handles, clicking the draft's last anchor
/// retracts its outgoing handle so the next segment starts straight,
/// Alt/Option-clicking an anchor toggles it between corner and smooth,
/// clicking the open endpoint of a selected path resumes drafting from it,
/// and Cmd/Ctrl restricts the gesture to point editing so a miss never
/// places an anchor.
class PenTool final : public Tool {
public:
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
  /// state before the pen session began - Escape / cancel never leaves a
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

  /// Remove the most recently placed anchor from the creation draft
  /// (Backspace/Delete while drafting). Removing the only remaining anchor
  /// discards the draft entirely. No-op during an active drag or a point-edit
  /// session on a committed path.
  ///
  /// @param editor Editor state and mutation queue.
  /// @return true if an anchor was removed (or the draft was discarded).
  bool removeLastAnchor(EditorApp& editor);

  /// Live rubber-band preview of the segment a click at `documentPoint`
  /// would commit: from the draft's last anchor (honoring its outgoing
  /// handle and the Shift 45-degree constraint) to the pointer - or to the
  /// first anchor when the pointer is within closing range. Returns nullopt
  /// while not drafting, mid-drag, in a point-edit session, or after close.
  ///
  /// @param documentPoint Pointer location in SVG document coordinates.
  /// @param modifiers Modifier-key state for the hover.
  [[nodiscard]] std::optional<Path> previewSegmentPath(const Vector2d& documentPoint,
                                                       const MouseModifiers& modifiers) const;

  /// True when a click at `documentPoint` would close the active contour
  /// (used for the close-path hover affordance).
  ///
  /// @param documentPoint Pointer location in SVG document coordinates.
  /// @param modifiers Modifier-key state for the hover.
  [[nodiscard]] bool wouldCloseAt(const Vector2d& documentPoint,
                                  const MouseModifiers& modifiers) const;

  /// First anchor of the active draft (the close-path target).
  [[nodiscard]] const Vector2d& draftStartPoint() const { return startPoint_; }

  /// Whether the tool is currently appending to or point-editing a path.
  [[nodiscard]] bool isDrafting() const { return activePath_.has_value(); }

  /// Whether a mouse drag is currently shaping a path point - the most
  /// recently placed anchor's handles, an existing anchor, or an existing
  /// control point.
  [[nodiscard]] bool isDraggingAnchor() const { return dragMode_ != DragMode::None; }

  /// Current path data for tests and UI diagnostics.
  [[nodiscard]] const std::string& activePathData() const { return activePathData_; }

private:
  struct Anchor {
    Vector2d point = Vector2d::Zero();
    std::optional<Vector2d> inHandle;
    std::optional<Vector2d> outHandle;
  };

  /// Anchors parsed from a selected `<path>`, plus whether its (single)
  /// contour is closed.
  struct SelectedPathState {
    std::vector<Anchor> anchors;
    bool closed = false;
  };

  /// What the active mouse drag is manipulating.
  enum class DragMode {
    None,              //!< No drag in progress.
    NewAnchorHandles,  //!< Shaping the just-placed anchor's mirrored handles.
    MoveAnchor,        //!< Moving an existing anchor (and its handles).
    MoveInHandle,      //!< Reshaping an existing anchor's incoming handle.
    MoveOutHandle,     //!< Reshaping an existing anchor's outgoing handle.
  };

  /// A hit on the editable point chrome.
  struct PointHit {
    DragMode mode = DragMode::None;
    std::size_t index = 0;
  };

  /// A hit on a path segment: the segment from anchor `index` to the next
  /// anchor (wrapping for closed contours), at curve parameter `t`.
  struct SegmentHit {
    std::size_t index = 0;
    double t = 0.0;
  };

  [[nodiscard]] std::optional<SelectedPathState> stateForSelectedPath(
      const EditorApp& editor) const;
  [[nodiscard]] Vector2d constrainedPoint(const Vector2d& documentPoint,
                                          const MouseModifiers& modifiers) const;
  [[nodiscard]] bool shouldCloseAt(const Vector2d& documentPoint,
                                   const MouseModifiers& modifiers) const;
  [[nodiscard]] std::string serializePathData() const;
  void startNewPath(EditorApp& editor, const Vector2d& documentPoint);
  void continueSelectedPath(EditorApp& editor, const svg::SVGPathElement& path,
                            const SelectedPathState& state, const Vector2d& documentPoint,
                            const MouseModifiers& modifiers);
  void rebuildActivePathData();
  void beginDragLastAnchor();
  [[nodiscard]] bool updateDraggedAnchor(const Vector2d& documentPoint, bool breakSymmetry,
                                         bool constrainAngle);
  void commitActivePathData(EditorApp& editor);
  void appendLine(EditorApp& editor, const Vector2d& documentPoint);
  /// Close the active contour at a click on the first anchor. Keeps the
  /// session alive so the held click can drag to shape the closing anchor's
  /// handles; mouse-up finalizes.
  void closePath(EditorApp& editor, const Vector2d& clickPoint, double toleranceDoc);

  /// Hit-test the given anchors against a document-space point using a
  /// screen-stable tolerance. Control points win over anchors only when
  /// strictly closer; zero-length handles are not hit targets.
  [[nodiscard]] static std::optional<PointHit> HitTestPoints(const std::vector<Anchor>& anchors,
                                                             const Vector2d& documentPoint,
                                                             double toleranceDoc);
  /// Hit-test the given anchors' segments (line or cubic, including the
  /// closing segment of a closed contour) against a document-space point.
  /// Returns the nearest segment hit within `toleranceDoc`, refined to the
  /// closest curve parameter.
  [[nodiscard]] static std::optional<SegmentHit> HitTestSegments(const std::vector<Anchor>& anchors,
                                                                 bool closed,
                                                                 const Vector2d& documentPoint,
                                                                 double toleranceDoc);
  /// Insert an anchor at the segment hit, splitting a cubic via de Casteljau
  /// (shape-preserving) or a line at its projection point.
  ///
  /// @return the inserted anchor's index.
  std::size_t insertAnchorAt(const SegmentHit& hit);
  /// Start a drag of an existing anchor or control point.
  void beginPointDrag(const PointHit& hit);
  /// Toggle the anchor at `index` between corner (no handles) and smooth
  /// (mirrored handles along the neighbor chord, one third of the distance to
  /// each neighbor).
  void convertAnchorSmoothness(std::size_t index);
  /// Apply pointer movement to the active MoveAnchor/Move*Handle drag.
  [[nodiscard]] bool updatePointDrag(const Vector2d& documentPoint,
                                     const MouseModifiers& modifiers);

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

  /// Active drag state. `dragIndex_` is the anchor being shaped/moved (or
  /// whose handle is being reshaped); `dragStartAnchor_` snapshots that anchor
  /// at mouse-down so constraints and aligned coupling compose from the
  /// original geometry instead of accumulating drift.
  DragMode dragMode_ = DragMode::None;
  std::size_t dragIndex_ = 0;
  bool draggedAnchorChanged_ = false;
  Anchor dragStartAnchor_;
  /// Whether the dragged anchor's handles were collinear (smooth) at
  /// mouse-down; decides aligned-vs-corner coupling for handle drags.
  bool dragStartAnchorSmooth_ = false;
  /// True while a point-edit session on a committed (non-drafting) selected
  /// path is active; finalized into one undoable command on mouse-up.
  bool editingExistingPath_ = false;
  /// Click (no drag) on the draft's last anchor retracts its outgoing handle.
  bool pendingRetractLastAnchor_ = false;
  /// Click (no drag) on a selected open path's endpoint resumes drafting.
  bool pendingResumeDraft_ = false;
  /// Alt/Option-click (no drag) on an anchor toggles corner/smooth.
  bool pendingConvertAnchor_ = false;
  /// The active drag is shaping the closing anchor after a close-path click;
  /// mouse-up finalizes the session.
  bool pendingFinalizeOnRelease_ = false;
  /// Document point of the close-path click; movement within
  /// `closeClickToleranceDoc_` of it is a plain click, not a shaping drag.
  Vector2d closeClickPoint_ = Vector2d::Zero();
  /// Hit tolerance (document units) captured with the close-path click.
  double closeClickToleranceDoc_ = 0.0;

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
