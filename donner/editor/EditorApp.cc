#include "donner/editor/EditorApp.h"

namespace donner::editor {

EditorApp::EditorApp() = default;

bool EditorApp::loadFromString(std::string_view svgBytes) {
  selectedEntity_ = entt::null;
  controller_.reset();
  return document_.loadFromString(svgBytes);
}

bool EditorApp::flushFrame() {
  return document_.flushFrame();
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
