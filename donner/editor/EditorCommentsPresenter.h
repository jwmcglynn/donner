#pragma once
/// @file

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "donner/base/Box.h"
#include "donner/editor/EditorCollaboration.h"
#include "donner/editor/ViewportState.h"

namespace donner::editor {

/// Native comment composer, feedback list and document-anchored UI pins.
class EditorCommentsPresenter {
public:
  /// Start a comment at an observed document point and optional element.
  void beginComment(EditorCollaboration& collaboration, Vector2d point,
                    std::optional<svg::SVGElement> element);
  /// Draw comment pins using the same viewport as the presented artwork.
  void drawPins(EditorCollaboration& collaboration, const ViewportState& viewport);
  /// Draw the non-modal comment composer and feedback list.
  void drawPanel(EditorCollaboration& collaboration, bool rendererIdle, const Box2d& initialBounds);
  /// Show or hide the feedback panel.
  void setVisible(bool visible) { visible_ = visible; }
  /// Whether the feedback panel is visible.
  bool visible() const { return visible_; }
  /// True when a point belongs to a comment button from the last presented frame.
  bool capturesInput(Vector2d screenPoint) const;
  /// Report a persistence failure while retaining the in-memory comments.
  void setPersistenceError(std::string error) { persistenceError_ = std::move(error); }

private:
  void drawComposer(EditorCollaboration& collaboration, bool rendererIdle);
  void drawCommentList(EditorCollaboration& collaboration);
  bool visible_ = false;
  bool focusComposer_ = false;
  std::optional<EditorComment> pendingAnchor_;
  std::array<char, 4097> draft_{};
  std::string error_;
  std::string persistenceError_;
  std::vector<Box2d> pinRegions_;
};
}  // namespace donner::editor
