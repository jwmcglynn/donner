#include "donner/editor/OverlayRenderer.h"

#include "donner/base/EcsRegistry.h"
#include "donner/base/Transform.h"
#include "donner/css/Color.h"
#include "donner/editor/EditorApp.h"
#include "donner/svg/components/shape/ShapeSystem.h"
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
  if (!editor.hasDocument() || !editor.hasSelection()) {
    return;
  }

  // Resolve the selected entity's world-space bounds via the same
  // ShapeSystem path that `SVGGeometryElement::worldBounds()` uses
  // internally. This bypasses `SVGElement`'s protected constructor while
  // still going through the canonical layout pipeline.
  const auto& constDoc = editor.document().document();
  // ShapeSystem::getShapeWorldBounds takes a non-const EntityHandle.
  // Const-cast is safe: we don't mutate the registry, ShapeSystem just
  // populates lazily-computed components which the renderer also touches.
  auto& mutableRegistry = const_cast<Registry&>(constDoc.registry());
  EntityHandle handle(mutableRegistry, editor.selectedEntity());
  if (!handle) {
    return;
  }

  const std::optional<Box2d> bounds =
      svg::components::ShapeSystem().getShapeWorldBounds(handle);
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
