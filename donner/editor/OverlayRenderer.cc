#include "donner/editor/OverlayRenderer.h"

#include "donner/base/Transform.h"
#include "donner/css/Color.h"
#include "donner/editor/EditorApp.h"
#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/properties/PaintServer.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/StrokeParams.h"

namespace donner::editor {

namespace {

constexpr double kSelectionStrokeWidth = 2.0;

svg::PaintParams MakeSelectionPaint() {
  svg::PaintParams paint;
  // Bright cyan stroke, no fill.
  paint.stroke = svg::PaintServer::Solid(
      css::Color(css::RGBA(0x00, 0xc8, 0xff, 0xff)));
  paint.fill = svg::PaintServer::None{};
  paint.strokeOpacity = 1.0;
  paint.strokeParams.strokeWidth = kSelectionStrokeWidth;
  paint.strokeParams.lineCap = svg::StrokeLinecap::Butt;
  paint.strokeParams.lineJoin = svg::StrokeLinejoin::Miter;
  paint.strokeParams.miterLimit = 4.0;
  return paint;
}

}  // namespace

void OverlayRenderer::drawChrome(svg::Renderer& renderer, const EditorApp& editor) {
  drawChrome(renderer, editor.selectedElement());
}

void OverlayRenderer::drawChrome(svg::Renderer& renderer,
                                 const std::optional<svg::SVGElement>& selection) {
  if (!selection.has_value()) {
    return;
  }

  // Resolve world-space bounds via the public `SVGGeometryElement`
  // API — no ECS reach-through. The selection is guaranteed to be a
  // geometry element because `SelectTool::onMouseDown` only selects
  // results returned by `EditorApp::hitTest`, which returns
  // `SVGGeometryElement`. Non-geometry selections (which can't happen
  // in M2 but might in future tools) are skipped.
  const svg::SVGElement& selected = *selection;
  if (!selected.isa<svg::SVGGeometryElement>()) {
    return;
  }
  const auto bounds = selected.cast<svg::SVGGeometryElement>().worldBounds();
  if (!bounds.has_value()) {
    return;
  }

  // Reset the renderer's transform to identity so the chrome rect is
  // drawn in document/world coordinates. The renderer's draw(document)
  // pass leaves an empty transform stack and identity current transform
  // after `endFrame`, but reset explicitly to be defensive against
  // backend-specific state.
  renderer.setTransform(Transform2d());
  renderer.setPaint(MakeSelectionPaint());
  renderer.drawRect(*bounds, MakeSelectionPaint().strokeParams);
}

}  // namespace donner::editor
