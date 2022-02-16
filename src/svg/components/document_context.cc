#include "src/svg/components/document_context.h"

#include "src/svg/components/circle_component.h"
#include "src/svg/components/computed_shadow_tree_component.h"
#include "src/svg/components/computed_style_component.h"
#include "src/svg/components/ellipse_component.h"
#include "src/svg/components/line_component.h"
#include "src/svg/components/path_component.h"
#include "src/svg/components/rect_component.h"
#include "src/svg/components/shadow_tree_component.h"
#include "src/svg/components/sized_element_component.h"
#include "src/svg/components/transform_component.h"

namespace donner::svg {

DocumentContext::DocumentContext(SVGDocument& document, Registry& registry)
    : document_(document), registry_(registry) {
  registry.on_construct<IdComponent>().connect<&DocumentContext::onIdSet>(this);
  registry.on_destroy<IdComponent>().connect<&DocumentContext::onIdDestroy>(this);

  // Set up render tree signals.
  entt::sink sink(computePaths_);
  sink.connect<&InstantiateComputedCircleComponents>();
  sink.connect<&InstantiateComputedEllipseComponents>();
  sink.connect<&InstantiateComputedPathComponents>();
  sink.connect<&InstantiateComputedRectComponents>();
  sink.connect<&InstantiateLineComponents>();
}

void DocumentContext::instantiateRenderTree(std::vector<ParseError>* outWarnings) {
  assert(defaultSize.has_value() && "defaultSize must be set before instantiating render tree");

  for (auto view = registry_.view<SizedElementComponent>(); auto entity : view) {
    auto [sizedElement] = view.get(entity);

    registry_.emplace_or_replace<ViewboxTransformComponent>(
        entity, sizedElement.computeTransform(registry_, entity, defaultSize.value()));
  }

  // Instantiate shadow trees.
  for (auto view = registry_.view<ShadowTreeComponent>(); auto entity : view) {
    auto [shadowTreeComponent] = view.get(entity);
    if (auto targetEntity = shadowTreeComponent.targetEntity(registry_);
        targetEntity != entt::null) {
      registry_.get_or_emplace<ComputedShadowTreeComponent>(entity).instantiate(
          registry_, targetEntity, outWarnings);
    } else if (outWarnings) {
      ParseError err;
      err.reason = std::string("Warning: Failed to resolve shadow tree target with href '") +
                   shadowTreeComponent.href() + "'";
      outWarnings->emplace_back(std::move(err));
    }
  }

  // Create placeholder ComputedStyleComponents for all elements in the tree.
  for (auto view = registry_.view<TreeComponent>(); auto entity : view) {
    std::ignore = registry_.get_or_emplace<ComputedStyleComponent>(entity);
  }

  // Compute the styles for all elements.
  for (auto view = registry_.view<ComputedStyleComponent>(); auto entity : view) {
    auto [styleComponent] = view.get(entity);
    styleComponent.computeProperties(EntityHandle(registry_, entity));
  }

  ComputeAllTransforms(registry_, outWarnings);

  computePaths_.publish(registry_, outWarnings);
}

}  // namespace donner::svg
