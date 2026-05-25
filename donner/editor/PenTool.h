#pragma once
/// @file
///
/// Prototype path authoring tool. `PenTool` creates source-backed `<path>`
/// elements and appends straight line segments from document-space clicks.

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
/// The prototype supports polyline creation, shift-constrained points,
/// continuing a selected open `<path>`, and closing an active path by
/// clicking near its first point.
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

  /// Handle pointer movement; currently unused by the prototype.
  ///
  /// @param editor Editor state and mutation queue.
  /// @param documentPoint Pointer location in SVG document coordinates.
  /// @param buttonHeld Whether the left mouse button is down.
  void onMouseMove(EditorApp& editor, const Vector2d& documentPoint, bool buttonHeld) override;

  /// Handle mouse release; currently unused by the prototype.
  ///
  /// @param editor Editor state and mutation queue.
  /// @param documentPoint Release location in SVG document coordinates.
  void onMouseUp(EditorApp& editor, const Vector2d& documentPoint) override;

  /// Cancel the in-progress path without modifying the current document.
  void cancel();

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
  void updateDraggedAnchor(const Vector2d& documentPoint);
  void commitActivePathData(EditorApp& editor);
  void appendLine(EditorApp& editor, const Vector2d& documentPoint);
  void closePath(EditorApp& editor);

  std::optional<svg::SVGPathElement> activePath_;
  std::vector<Anchor> anchors_;
  Vector2d startPoint_ = Vector2d::Zero();
  Vector2d currentPoint_ = Vector2d::Zero();
  std::string activePathData_;
  bool closed_ = false;
  bool draggingAnchor_ = false;
  bool draggedAnchorChanged_ = false;
  std::size_t draggingAnchorIndex_ = 0;
};

}  // namespace donner::editor
