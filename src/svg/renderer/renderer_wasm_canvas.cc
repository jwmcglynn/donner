#include "src/svg/renderer/renderer_wasm_canvas.h"

#include "src/svg/components/computed_shadow_tree_component.h"
#include "src/svg/components/computed_style_component.h"
#include "src/svg/components/gradient_component.h"
#include "src/svg/components/id_component.h"  // For verbose logging.
#include "src/svg/components/linear_gradient_component.h"
#include "src/svg/components/path_component.h"
#include "src/svg/components/path_length_component.h"
#include "src/svg/components/pattern_component.h"
#include "src/svg/components/preserve_aspect_ratio_component.h"
#include "src/svg/components/radial_gradient_component.h"
#include "src/svg/components/rect_component.h"
#include "src/svg/components/rendering_behavior_component.h"
#include "src/svg/components/rendering_instance_component.h"
#include "src/svg/components/shadow_entity_component.h"
#include "src/svg/components/sized_element_component.h"
#include "src/svg/components/transform_component.h"
#include "src/svg/components/tree_component.h"
#include "src/svg/components/viewbox_component.h"
#include "src/svg/renderer/renderer_utils.h"

namespace donner::svg {

namespace {

struct InstanceView {
public:
  using ViewType =
      entt::basic_view<Entity, entt::get_t<RenderingInstanceComponent>, entt::exclude_t<>, void>;
  using Iterator = ViewType::iterator;

  explicit InstanceView(const ViewType& view)
      : view_(view), current_(view.begin()), end_(view.end()) {}

  bool done() const { return current_ == end_; }
  void advance() { ++current_; }

  Entity currentEntity() const {
    assert(!done());
    return *current_;
  }

  const RenderingInstanceComponent& get() const {
    return view_.get<RenderingInstanceComponent>(currentEntity());
  }

private:
  const ViewType& view_;
  Iterator current_;
  Iterator end_;
};

}  // namespace

class RendererWasmCanvas::Impl {
public:
  Impl(RendererWasmCanvas& renderer, InstanceView&& view)
      : renderer_(renderer), ctx_(renderer.canvas_.getContext2D()), view_(std::move(view)) {}

  void drawUntil(Registry& registry, Entity endEntity) {
    bool foundEndEntity = false;

    while (!view_.done() && !foundEndEntity) {
      foundEndEntity = view_.currentEntity() == endEntity;

      const RenderingInstanceComponent& instance = view_.get();
      [[maybe_unused]] const Entity entity = view_.currentEntity();
      view_.advance();

      if (renderer_.verbose_) {
        std::cout << "Rendering "
                  << TypeToString(registry.get<TreeComponent>(instance.dataEntity).type()) << " ";

        if (const auto* idComponent = registry.try_get<IdComponent>(instance.dataEntity)) {
          std::cout << "id=" << idComponent->id << " ";
        }

        std::cout << instance.dataEntity;
        if (instance.isShadow(registry)) {
          std::cout << " (shadow " << instance.styleHandle(registry).entity() << ")";
        }

        std::cout << " transform=" << instance.transformCanvasSpace << std::endl;

        std::cout << std::endl;
      }

      const ComputedStyleComponent& styleComponent =
          instance.styleHandle(registry).get<ComputedStyleComponent>();

      if (instance.visible) {
        if (const auto* path = instance.dataHandle(registry).try_get<ComputedPathComponent>()) {
          drawPath(instance.dataHandle(registry), instance, *path, styleComponent.properties(),
                   styleComponent.viewbox(), FontMetrics());
        }
      }
    }
  }

  void drawPath(EntityHandle dataHandle, const RenderingInstanceComponent& instance,
                const ComputedPathComponent& path, const PropertyRegistry& style,
                const Boxd& viewbox, const FontMetrics& fontMetrics) {
    if (HasPaint(instance.resolvedFill)) {
      drawPathFill(dataHandle, path, instance.resolvedFill, style, viewbox);
    }

    if (HasPaint(instance.resolvedStroke)) {
      drawPathStroke(dataHandle, path, instance.resolvedStroke, style, viewbox, fontMetrics);
    }
  }

  void drawPathFill(EntityHandle dataHandle, const ComputedPathComponent& path,
                    const ResolvedPaintServer& paint, const PropertyRegistry& style,
                    const Boxd& viewbox) {
    if (const auto* solid = std::get_if<PaintServer::Solid>(&paint)) {
      ctx_.setFillStyle("#00FF00");
      // skPaint.setColor(toSkia(solid->color.resolve(style.color.getRequired().rgba(),
      // fillOpacity)));

      ctx_.fill(path.spline);
    }
  }

  void drawPathStroke(EntityHandle dataHandle, const ComputedPathComponent& path,
                      const ResolvedPaintServer& paint, const PropertyRegistry& style,
                      const Boxd& viewbox, const FontMetrics& fontMetrics) {
    if (const auto* solid = std::get_if<PaintServer::Solid>(&paint)) {
      ctx_.setStrokeStyle("#00FF00");

      // skPaint.setColor(
      //     toSkia(solid->color.resolve(style.color.getRequired().rgba(), strokeOpacity)));

      ctx_.stroke(path.spline);
    }
  }

private:
  RendererWasmCanvas& renderer_;
  canvas::CanvasRenderingContext2D ctx_;
  InstanceView view_;

  std::vector<SubtreeInfo> subtreeMarkers_;
};

RendererWasmCanvas::RendererWasmCanvas(std::string_view canvasId, bool verbose)
    : verbose_(verbose), canvas_(canvas::Canvas::Create(canvasId)) {}

RendererWasmCanvas::~RendererWasmCanvas() = default;

void RendererWasmCanvas::draw(SVGDocument& document) {
  Registry& registry = document.registry();
  const Entity rootEntity = document.rootEntity();

  // TODO: Plumb outWarnings.
  RendererUtils::prepareDocumentForRendering(document, verbose_);

  const Vector2i renderingSize =
      registry.get<SizedElementComponent>(rootEntity)
          .calculateViewportScaledDocumentSize(registry, InvalidSizeBehavior::ReturnDefault);

  canvas_.setSize(renderingSize);

  draw(registry, rootEntity);
}

int RendererWasmCanvas::width() const {
  return canvas_.size().x;
}

int RendererWasmCanvas::height() const {
  return canvas_.size().y;
}

void RendererWasmCanvas::draw(Registry& registry, Entity root) {
  Impl impl(*this, InstanceView{registry.view<RenderingInstanceComponent>()});
  impl.drawUntil(registry, entt::null);
}

}  // namespace donner::svg
