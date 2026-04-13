#include "donner/editor/EditorApp.h"

namespace donner::editor {

EditorApp::EditorApp() = default;

bool EditorApp::loadFromString(std::string_view svgBytes) {
  selectedElement_.reset();
  controller_.reset();
  undoTimeline_.clear();
  const bool result = document_.loadFromString(svgBytes);
  // A successful load resets the dirty state — the in-memory document
  // now matches the last-loaded bytes. `setCurrentFilePath` should be
  // called separately by the caller if the bytes came from a file.
  if (result) {
    isDirty_ = false;
  }
  return result;
}

bool EditorApp::flushFrame() {
  return document_.flushFrame();
}

void EditorApp::undo() {
  auto snapshot = undoTimeline_.undo();
  if (!snapshot.has_value()) {
    return;
  }

  // Route the restored transform through the command queue so every
  // DOM write — tool drags, text-pane re-parse, and undo — goes through
  // the same mutation seam. The queue coalesces with any pending
  // commands and applies on the next `flushFrame()`.
  applyMutation(EditorCommand::SetTransformCommand(snapshot->element, snapshot->transform));
}

void EditorApp::redo() {
  // "Redo" in the non-destructive timeline model is "break the active
  // undo chain and undo again". Breaking the chain causes the next
  // undo to start a fresh chain from the end of the timeline, which
  // means the first entry it walks is the most recently-appended
  // undo-entry — whose `before` state is the post-drag position.
  undoTimeline_.breakUndoChain();
  undo();
}

std::optional<svg::SVGGeometryElement> EditorApp::hitTest(const Vector2d& documentPoint) {
  if (!document_.hasDocument()) {
    return std::nullopt;
  }

  // Rebuild the DonnerController whenever the document version advances
  // past the version we built it for. The controller copies the SVGDocument
  // handle (which internally shares the registry), so reconstruction is
  // cheap relative to a full re-parse.
  const auto currentVersion = document_.currentFrameVersion();
  if (!controller_.has_value() || controllerVersion_ != currentVersion) {
    controller_.emplace(document_.document());
    controllerVersion_ = currentVersion;
  }

  return controller_->findIntersecting(documentPoint);
}

}  // namespace donner::editor
