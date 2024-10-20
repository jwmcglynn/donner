#include "donner/svg/renderer/RendererWasmCanvas.h"

#include "donner/svg/components/ElementTypeComponent.h"
#include "donner/svg/components/IdComponent.h"  // For verbose logging.
#include "donner/svg/components/PathLengthComponent.h"
#include "donner/svg/components/PreserveAspectRatioComponent.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/layout/SizedElementComponent.h"
#include "donner/svg/components/layout/TransformComponent.h"
#include "donner/svg/components/paint/PatternComponent.h"
#include "donner/svg/components/shadow/ShadowEntityComponent.h"
#include "donner/svg/components/shape/ComputedPathComponent.h"
#include "donner/svg/components/shape/PathComponent.h"
#include "donner/svg/components/shape/RectComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/renderer/RendererUtils.h"
#include "donner/svg/renderer/common/RenderingInstanceView.h"

namespace donner::svg {

class RendererWasmCanvas::Impl {
public:
  Impl(RendererWasmCanvas& renderer, RenderingInstanceView&& view)
      : renderer_(renderer), ctx_(renderer.canvas_.getContext2D()), view_(std::move(view)) {}

  void drawUntil(Registry& registry, Entity endEntity) {
    bool foundEndEntity = false;

    while (!view_.done() && !foundEndEntity) {
      foundEndEntity = view_.currentEntity() == endEntity;

      const components::RenderingInstanceComponent& instance = view_.get();
      [[maybe_unused]] const Entity entity = view_.currentEntity();
      view_.advance();

      if (renderer_.verbose_) {
        std::cout << "Rendering "
                  << registry.get<components::ElementTypeComponent>(instance.dataEntity).type()
                  << " ";

        if (const auto* idComponent =
                registry.try_get<components::IdComponent>(instance.dataEntity)) {
          std::cout << "id=" << idComponent->id << " ";
        }

        std::cout << instance.dataEntity;
        if (instance.isShadow(registry)) {
          std::cout << " (shadow " << instance.styleHandle(registry).entity() << ")";
        }

        std::cout << " transform=" << instance.entityFromWorldTransform << std::endl;

        std::cout << "\n";
      }

      const auto& styleComponent =
          instance.styleHandle(registry).get<components::ComputedStyleComponent>();

      if (instance.visible) {
        if (const auto* path =
                instance.dataHandle(registry).try_get<components::ComputedPathComponent>()) {
          drawPath(
              instance.dataHandle(registry), instance, *path, styleComponent.properties.value(),
              components::LayoutSystem().getViewport(instance.dataHandle(registry)), FontMetrics());
        }
      }
    }
  }

  void drawPath(EntityHandle dataHandle, const components::RenderingInstanceComponent& instance,
                const components::ComputedPathComponent& path, const PropertyRegistry& style,
                const Boxd& viewbox, const FontMetrics& fontMetrics) {
    if (HasPaint(instance.resolvedFill)) {
      drawPathFill(dataHandle, path, instance.resolvedFill, style, viewbox);
    }

    if (HasPaint(instance.resolvedStroke)) {
      drawPathStroke(dataHandle, path, instance.resolvedStroke, style, viewbox, fontMetrics);
    }
  }

  void drawPathFill(EntityHandle dataHandle, const components::ComputedPathComponent& path,
                    const components::ResolvedPaintServer& paint, const PropertyRegistry& style,
                    const Boxd& viewbox) {
    const float fillOpacity = NarrowToFloat(style.fillOpacity.get().value());
    if (const auto* solid = std::get_if<PaintServer::Solid>(&paint)) {
      ctx_.setFillStyle(
          solid->color.resolve(style.color.getRequired().rgba(), fillOpacity).toHexString());

      ctx_.fill(path.spline);
    }
  }

  void drawPathStroke(EntityHandle dataHandle, const components::ComputedPathComponent& path,
                      const components::ResolvedPaintServer& paint, const PropertyRegistry& style,
                      const Boxd& viewbox, const FontMetrics& fontMetrics) {
    const float strokeOpacity = NarrowToFloat(style.strokeOpacity.get().value());

    if (const auto* solid = std::get_if<PaintServer::Solid>(&paint)) {
      ctx_.setStrokeStyle(
          solid->color.resolve(style.color.getRequired().rgba(), strokeOpacity).toHexString());

      ctx_.stroke(path.spline);
    }
  }

private:
  RendererWasmCanvas& renderer_;
  canvas::CanvasRenderingContext2D ctx_;
  RenderingInstanceView view_;

  std::vector<components::SubtreeInfo> subtreeMarkers_;
};

RendererWasmCanvas::RendererWasmCanvas(std::string_view canvasId, bool verbose)
    : verbose_(verbose), canvas_(canvas::Canvas::Create(canvasId)) {}

RendererWasmCanvas::~RendererWasmCanvas() = default;

void RendererWasmCanvas::draw(SVGDocument& document) {
  // TODO: Plumb outWarnings.
  RendererUtils::prepareDocumentForRendering(document, verbose_);

  const Vector2i renderingSize = document.canvasSize();

  canvas_.setSize(renderingSize);

  draw(document.registry());
}

int RendererWasmCanvas::width() const {
  return canvas_.size().x;
}

int RendererWasmCanvas::height() const {
  return canvas_.size().y;
}

void RendererWasmCanvas::draw(Registry& registry) {
  Impl impl(*this, RenderingInstanceView{registry});
  impl.drawUntil(registry, entt::null);
}

}  // namespace donner::svg
