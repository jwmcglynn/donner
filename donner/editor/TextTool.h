#pragma once
/// @file
///
/// `TextTool` is the editor's text-authoring tool, following the standard
/// design-tool contract: a click on existing text opens an in-canvas editing
/// session on that element with the caret at the clicked character, a
/// click-drag on empty canvas draws a text box, and a double-click on empty
/// canvas starts point text (a plain click on empty canvas creates nothing -
/// it only commits any active session). Typing, Enter (hard line break),
/// Backspace/Delete, and caret movement edit the live `<text>` element
/// through the DOM mutation seam; box text wraps greedily to the box width
/// using the engine's measured character geometry. Cmd/Ctrl+B/I/U toggle
/// bold, italic, and underline on the element. Escape, clicking away, or
/// switching tools commits the session as a single undoable operation (an
/// empty session on a newly created element deletes it, leaving the document
/// unchanged; emptying an existing element deletes it as an undoable edit).

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/editor/SelectionTransformHandles.h"
#include "donner/editor/Tool.h"
#include "donner/svg/SVGTextElement.h"

namespace donner::editor {

/// In-canvas text authoring with a live caret.
class TextTool final : public Tool {
public:
  /// Default `font-family` attribute applied to new text.
  static constexpr std::string_view kDefaultFontFamily = "sans-serif";
  /// Default `font-size` attribute applied to new text (document units).
  static constexpr double kDefaultFontSize = 32.0;
  /// Default fill applied to new text, written as `style="fill: ..."`.
  static constexpr std::string_view kDefaultFill = "black";
  /// Line height as a multiple of font size for wrapped/multi-line text.
  static constexpr double kLineHeightFactor = 1.2;
  /// Minimum drag extent (document units, at zoom 1) that turns a click into
  /// a text-box drag instead of point text.
  static constexpr double kBoxDragScreenTolerance = 4.0;

  /// Caret movement gestures.
  enum class CaretMove {
    Left,
    Right,
    Up,
    Down,
    LineStart,
    LineEnd,
  };

  /// Caret + frame chrome for the active editing session, in document space.
  struct EditingChrome {
    /// Caret line endpoints (top, bottom).
    Vector2d caretTopDoc;
    Vector2d caretBottomDoc;
    /// Text-box frame for box text (absent for point text).
    std::optional<Box2d> boxDoc;
  };

  /// Begin a gesture: a press on the session frame's transform handles
  /// starts a frame resize (reflow - glyphs never scale) or a rotate; a
  /// click inside the session's text moves the caret (clicks inside the
  /// frame but off every glyph land the caret at the end); otherwise commit
  /// any active session, then either open an editing session on the
  /// existing `<text>` under the click (caret at the clicked character) or
  /// start a box drag on empty canvas.
  void onMouseDown(EditorApp& editor, const Vector2d& documentPoint,
                   MouseModifiers modifiers) override;

  /// Extend the box drag or the active frame gesture.
  void onMouseMove(EditorApp& editor, const Vector2d& documentPoint, bool buttonHeld) override;

  /// Finish the gesture: a drag creates box text and a double-click places
  /// point text, opening the editing session with the caret at the start; a
  /// plain click on empty canvas creates nothing. Releasing a frame gesture
  /// keeps the editing session open.
  void onMouseUp(EditorApp& editor, const Vector2d& documentPoint) override;

  /// True while an in-canvas editing session (with caret) is active.
  [[nodiscard]] bool isEditing() const { return state_ == State::Editing; }
  /// True while a text-box drag is in progress.
  [[nodiscard]] bool isDraggingBox() const { return state_ == State::DraggingBox; }
  /// True while a frame handle gesture (frame resize or rotate) is active.
  /// The editing session stays open underneath it.
  [[nodiscard]] bool isAdjustingFrame() const { return frameGesture_ != FrameGesture::None; }
  /// True while the active frame gesture is a rotate (subset of
  /// `isAdjustingFrame`); drives the rotate cursor during the drag.
  [[nodiscard]] bool isRotatingFrame() const { return frameGesture_ == FrameGesture::Rotate; }
  /// Corner of the active frame gesture, for cursor feedback.
  [[nodiscard]] SelectionTransformCorner frameCorner() const { return frameCorner_; }

  /// Hit-test the session frame's transform handles at @p documentPoint for
  /// hover cursor feedback: `Resize` over a corner handle, `Rotate` in the
  /// ring outside it, `None` elsewhere or when no session is editing. Reads
  /// the session's computed ink bounds for point text - call only while the
  /// async render worker is idle.
  [[nodiscard]] SelectionTransformHandleIntent frameHandleIntentAt(const Vector2d& documentPoint,
                                                                   double pixelsPerDocUnit,
                                                                   bool includeRotate) const;
  /// The box being dragged, for drag-preview chrome.
  [[nodiscard]] const std::optional<Box2d>& dragBoxDoc() const { return dragBoxDoc_; }

  /// Insert one code point at the caret.
  void insertCodepoint(EditorApp& editor, char32_t codepoint);
  /// Insert a hard line break at the caret.
  void insertNewline(EditorApp& editor);
  /// Delete the code point before the caret.
  void backspace(EditorApp& editor);
  /// Delete the code point after the caret.
  void deleteForward(EditorApp& editor);
  /// Move the caret.
  void moveCaret(EditorApp& editor, CaretMove move);

  /// Toggle `font-weight: bold` on the session's text element.
  void toggleBold(EditorApp& editor);
  /// Toggle `font-style: italic` on the session's text element.
  void toggleItalic(EditorApp& editor);
  /// Toggle `text-decoration: underline` on the session's text element.
  void toggleUnderline(EditorApp& editor);

  /// Commit the active session as one undoable operation. An empty session
  /// deletes the created element, restoring the pre-session document and
  /// selection.
  ///
  /// @return true if a session was committed (or discarded-as-empty).
  bool commit(EditorApp& editor);

  /// Reset tool state without touching the document (tool destruction).
  void cancel();

  /// Caret + frame chrome for the active session, or nullopt when not
  /// editing. Reads the element's computed character geometry - call only
  /// while the async render worker is idle.
  [[nodiscard]] std::optional<EditingChrome> editingChrome(EditorApp& editor) const;

  /// Logical content of the active session (hard breaks as '\n'); for tests.
  [[nodiscard]] const std::u32string& sessionContent() const { return content_; }
  /// Caret position in code points; for tests.
  [[nodiscard]] std::size_t caretIndex() const { return caretIndex_; }

private:
  enum class State {
    Idle,         //!< No session.
    DraggingBox,  //!< Mouse held, dragging out a text box.
    Editing,      //!< In-canvas editing session with caret.
  };

  /// Create the session `<text>` element (point or box) and enter Editing.
  void beginEditingSession(EditorApp& editor, const Vector2d& originDoc,
                           const std::optional<Box2d>& boxDoc);
  /// Open an editing session on an existing `<text>` element: reconstruct the
  /// logical content from its DOM children and place the caret at the clicked
  /// character (@p documentPoint), or at the end when the click misses every
  /// glyph.
  void beginEditingSessionForExisting(EditorApp& editor, const svg::SVGTextElement& text,
                                      const Vector2d& documentPoint);
  /// Caret index for a click at @p documentPoint inside the session's text,
  /// or nullopt when the click misses every glyph. Clicks in the trailing
  /// half of a glyph place the caret after it.
  [[nodiscard]] std::optional<std::size_t> caretIndexAtPoint(const Vector2d& documentPoint) const;
  /// The session frame in the text's local space: the authored box for box
  /// text, or the laid-out ink bounds for point text. Nullopt when the ink
  /// is empty and no box is authored.
  [[nodiscard]] std::optional<Box2d> sessionFrameLocal() const;
  /// Start a frame resize/rotate when @p documentPoint lands on the session
  /// frame's transform handles. Returns true when a gesture began.
  bool beginFrameGestureAtPoint(const Vector2d& documentPoint, MouseModifiers modifiers);
  /// Advance the active frame gesture to @p documentPoint: resize rewrites
  /// the box attributes and rewraps (converting point text to a user-sized
  /// box on the first move); rotate sets the element transform.
  void updateFrameGesture(EditorApp& editor, const Vector2d& documentPoint);
  /// Rebuild the element's content from `content_` (single text node for one
  /// line; one `<tspan>` per line otherwise), wrapping box text to the box
  /// width using measured character advances. Flushes so subsequent
  /// character-geometry reads see the new content.
  void syncContentToDom(EditorApp& editor);
  /// Greedy-wrap `content_` into display lines: hard breaks always split;
  /// box text also splits at word boundaries when the measured line width
  /// exceeds the box. Returns the caret's (line, column) as a side effect of
  /// the display-line structure.
  [[nodiscard]] std::vector<std::u32string> displayLines(EditorApp& editor) const;
  /// Measured widths for each code point of `content_` (excluding '\n'),
  /// from the flushed element's character geometry; empty when unavailable.
  [[nodiscard]] std::vector<double> measureCharacterWidths(EditorApp& editor) const;
  /// Map the caret index into (displayLine, column) against `lines`.
  [[nodiscard]] std::pair<std::size_t, std::size_t> caretLineColumn(
      const std::vector<std::u32string>& lines) const;
  /// Record the pre-session source baseline for the single session undo.
  void beginSessionUndo(EditorApp& editor);

  /// Active frame handle gesture (session stays in Editing underneath).
  enum class FrameGesture {
    None,    //!< No frame gesture.
    Resize,  //!< Corner handle drag resizes the frame (reflow, never scale).
    Rotate,  //!< Rotate ring drag rotates the element.
  };

  State state_ = State::Idle;
  FrameGesture frameGesture_ = FrameGesture::None;
  /// Grabbed corner for a frame resize.
  SelectionTransformCorner frameCorner_ = SelectionTransformCorner::BottomRight;
  /// Frame rect (text-local space) captured at frame-gesture start.
  Box2d frameStartLocal_ = Box2d(Vector2d::Zero(), Vector2d::Zero());
  /// `documentFromText_` captured at frame-gesture start; the live value
  /// changes under a rotate.
  Transform2d frameStartDocumentFromText_;
  /// Element transform captured at rotate start.
  Transform2d rotateStartTransform_;
  /// Pointer angle around the frame center at rotate start (radians).
  double rotateStartAngleRadians_ = 0.0;
  /// Frame center (document space) the rotate pivots around.
  Vector2d rotateCenterDoc_ = Vector2d::Zero();
  /// The session's `<text>` element (valid in Editing).
  std::optional<svg::SVGTextElement> sessionText_;
  /// True when the session created its `<text>` element (a click-away with
  /// empty content silently deletes it); false when the session edits an
  /// existing element (emptying it deletes it as an undoable edit).
  bool createdBySession_ = true;
  /// Maps the session text's local coordinates (where the `x`/`y`
  /// attributes, character geometry, and box dimensions live) into document
  /// space. Identity for elements the session creates; the element's
  /// `elementFromWorld()` when editing existing (possibly transformed) text.
  Transform2d documentFromText_;
  /// Box frame for box text (text-local space); absent for point text.
  std::optional<Box2d> boxText_;
  /// Live drag rectangle while in DraggingBox.
  std::optional<Box2d> dragBoxDoc_;
  Vector2d dragStartDoc_ = Vector2d::Zero();
  double dragToleranceDoc_ = kBoxDragScreenTolerance;
  /// True when the buffered mouse-down was a double-click: releasing without
  /// a box drag places point text instead of doing nothing.
  bool pendingDoubleClick_ = false;
  /// Text origin in the session text's local space: `x` attribute and
  /// first-line baseline `y` attribute.
  Vector2d originText_ = Vector2d::Zero();
  /// Logical content with '\n' hard breaks, in code points.
  std::u32string content_;
  /// Caret position in code points (0..content_.size()).
  std::size_t caretIndex_ = 0;
  /// Font size in document units (drives baseline offset and line height).
  double fontSize_ = kDefaultFontSize;
  /// Cached per-code-point widths from the last sync, for wrapping.
  std::vector<double> cachedCharWidths_;
  /// Pre-session source for the single finalize undo entry.
  std::optional<std::string> sessionBeforeSource_;
  /// Selection to restore when an empty session is discarded.
  std::vector<svg::SVGElement> previousSelection_;
};

}  // namespace donner::editor
