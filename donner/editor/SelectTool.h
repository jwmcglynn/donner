#pragma once
/// @file
///
/// `SelectTool` is the editor's first and (in this milestone) only tool.
/// It hit-tests the document on mouse-down, sets the editor selection, and
/// translates the selected element by the cumulative drag delta on
/// mouse-move while the button is held. All DOM mutations flow through
/// `EditorApp::applyMutation()` as `EditorCommand::SetTransform` — never
/// directly.

#include <optional>

#include "donner/base/EcsRegistry.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/editor/Tool.h"

namespace donner::editor {

class SelectTool final : public Tool {
public:
  void onMouseDown(EditorApp& editor, const Vector2d& documentPoint) override;
  void onMouseMove(EditorApp& editor, const Vector2d& documentPoint, bool buttonHeld) override;
  void onMouseUp(EditorApp& editor, const Vector2d& documentPoint) override;

  /// Whether a drag is currently in progress (button is held after a
  /// successful hit-test on mouse-down).
  [[nodiscard]] bool isDragging() const { return dragState_.has_value(); }

private:
  struct DragState {
    Entity entity = entt::null;
    Vector2d startDocumentPoint;
    Transform2d startTransform;
  };

  std::optional<DragState> dragState_;
};

}  // namespace donner::editor
