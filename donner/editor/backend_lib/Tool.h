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

/// Modifier-key state captured at the moment a mouse event was
/// dispatched. Tools use this for shift-add-to-selection, constrained
/// resize, alt-clone, etc. Default-constructed = no modifiers, which keeps existing
/// callsites that don't care about modifiers source-compatible.
struct MouseModifiers {
  /// Shift held — used by `SelectTool` to toggle/extend selection or constrain resize.
  bool shift = false;
};

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

  /// Mouse button (left) was pressed at `documentPoint`. `modifiers`
  /// captures the modifier-key state at the moment of the press;
  /// implementations that don't care can ignore it. Default arguments
  /// on virtuals are banned by the style guide, so callers that don't
  /// need modifier state should pass `MouseModifiers{}` explicitly.
  virtual void onMouseDown(EditorApp& editor, const Vector2d& documentPoint,
                           MouseModifiers modifiers) = 0;

  /// Mouse moved to `documentPoint`. `buttonHeld` is true while the left
  /// button is down — i.e., this is a drag continuation rather than a
  /// hover.
  virtual void onMouseMove(EditorApp& editor, const Vector2d& documentPoint, bool buttonHeld) = 0;

  /// Mouse button was released at `documentPoint`.
  virtual void onMouseUp(EditorApp& editor, const Vector2d& documentPoint) = 0;
};

}  // namespace donner::editor
