#pragma once
/// @file
///
/// `Tool` is the abstract interface for editor pointer tools (Select,
/// future Path, future Node-edit, etc.). Tools observe the editor state via
/// the `EditorApp&` parameter and produce DOM mutations exclusively by
/// calling `EditorApp::applyMutation()` — they never touch the DOM directly.
///
/// Coordinates passed to tool methods are in **document space** (the same
/// coordinate system as the SVG canvas). Coordinate translation from
/// screen space happens at the main-loop layer using
/// `donner::editor::ViewportGeometry`, so tools are insulated from the
/// viewport / pan / zoom state.

#include "donner/base/Vector2.h"

namespace donner::editor {

class EditorApp;

/// Abstract editor pointer tool. Implementations are stateless across
/// document load (the editor recreates them on document change is fine
/// since `Tool` instances are cheap), but may carry per-drag state.
class Tool {
public:
  Tool() = default;
  virtual ~Tool() = default;

  Tool(const Tool&) = delete;
  Tool& operator=(const Tool&) = delete;
  Tool(Tool&&) = delete;
  Tool& operator=(Tool&&) = delete;

  /// Mouse button (left) was pressed at `documentPoint`.
  virtual void onMouseDown(EditorApp& editor, const Vector2d& documentPoint) = 0;

  /// Mouse moved to `documentPoint`. `buttonHeld` is true while the left
  /// button is down — i.e., this is a drag continuation rather than a
  /// hover.
  virtual void onMouseMove(EditorApp& editor, const Vector2d& documentPoint, bool buttonHeld) = 0;

  /// Mouse button was released at `documentPoint`.
  virtual void onMouseUp(EditorApp& editor, const Vector2d& documentPoint) = 0;
};

}  // namespace donner::editor
