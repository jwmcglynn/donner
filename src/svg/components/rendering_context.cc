#include "src/svg/components/rendering_context.h"

#include <cassert>

#include "src/svg/components/circle_component.h"
#include "src/svg/components/computed_shadow_tree_component.h"
#include "src/svg/components/computed_style_component.h"
#include "src/svg/components/ellipse_component.h"
#include "src/svg/components/gradient_component.h"
#include "src/svg/components/line_component.h"
#include "src/svg/components/path_component.h"
#include "src/svg/components/poly_component.h"
#include "src/svg/components/rect_component.h"
#include "src/svg/components/shadow_tree_component.h"
#include "src/svg/components/sized_element_component.h"
#include "src/svg/components/stop_component.h"
#include "src/svg/components/transform_component.h"

namespace donner::svg {

RenderingContext::RenderingContext(Registry& registry) : registry_(registry) {
  // Set up render tree signals.
  {
    entt::sink sink(evaluateConditionalComponents_);
    sink.connect<&EvaluateConditionalGradientShadowTrees>();
  }

  {
    entt::sink sink(instantiateComputedComponents_);
    sink.connect<&InstantiateComputedCircleComponents>();
    sink.connect<&InstantiateComputedEllipseComponents>();
    sink.connect<&InstantiateComputedPathComponents>();
    sink.connect<&InstantiateComputedRectComponents>();
    sink.connect<&InstantiateLineComponents>();
    sink.connect<&InstantiatePolyComponents>();

    // Should instantiate <stop> before gradients.
    sink.connect<&InstantiateStopComponents>();
    sink.connect<&InstantiateGradientComponents>();
  }
}

void RenderingContext::instantiateRenderTree(std::vector<ParseError>* outWarnings) {
  // Instantiate shadow trees.
  for (auto view = registry_.view<ShadowTreeComponent>(); auto entity : view) {
    auto [shadowTreeComponent] = view.get(entity);
    if (auto targetEntity = shadowTreeComponent.targetEntity(registry_)) {
      registry_.get_or_emplace<ComputedShadowTreeComponent>(entity).instantiate(
          registry_, targetEntity.value(), outWarnings);
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

  for (auto view = registry_.view<SizedElementComponent, ComputedStyleComponent>();
       auto entity : view) {
    auto [component, style] = view.get(entity);
    component.computeWithPrecomputedStyle(EntityHandle(registry_, entity), style, FontMetrics(),
                                          outWarnings);
  }

  ComputeAllTransforms(registry_, outWarnings);

  evaluateConditionalComponents_.publish(registry_);
  instantiateComputedComponents_.publish(registry_, outWarnings);
}

}  // namespace donner::svg
