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
#include "src/svg/components/rendering_behavior_component.h"
#include "src/svg/components/rendering_instance_component.h"
#include "src/svg/components/shadow_tree_component.h"
#include "src/svg/components/sized_element_component.h"
#include "src/svg/components/stop_component.h"
#include "src/svg/components/transform_component.h"

namespace donner::svg {

namespace {

/**
 * The current value of the context-fill and context-stroke paint servers, based on the rules
 * described here: https://www.w3.org/TR/SVG2/painting.html#SpecifyingPaint
 */
struct ContextPaintServers {
  ResolvedPaintServer contextFill = PaintServer::None();
  ResolvedPaintServer contextStroke = PaintServer::None();
};

ResolvedPaintServer ResolvePaint(EntityHandle dataHandle, const PaintServer& paint,
                                 const ContextPaintServers& contextPaintServers) {
  if (paint.is<PaintServer::Solid>()) {
    return paint.get<PaintServer::Solid>();
  } else if (paint.is<PaintServer::ElementReference>()) {
    const PaintServer::ElementReference& ref = paint.get<PaintServer::ElementReference>();

    if (auto resolvedRef = ref.reference.resolve(*dataHandle.registry())) {
      return PaintResolvedReference{resolvedRef.value(), ref.fallback};
    } else if (ref.fallback) {
      return PaintServer::Solid(ref.fallback.value());
    } else {
      return PaintServer::None();
    }
  } else if (paint.is<PaintServer::ContextFill>()) {
    return contextPaintServers.contextFill;
  } else if (paint.is<PaintServer::ContextStroke>()) {
    return contextPaintServers.contextStroke;
  } else {
    return PaintServer::None();
  }
}

}  // namespace

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
  createComputedComponents(outWarnings);
  instantiateRenderTreeWithPrecomputedTree();
}

void RenderingContext::createComputedComponents(std::vector<ParseError>* outWarnings) {
  // Evaluate conditional components which may create shadow trees.
  evaluateConditionalComponents_.publish(registry_);

  // Instantiate shadow trees.
  for (auto view = registry_.view<ShadowTreeComponent>(); auto entity : view) {
    auto [shadowTreeComponent] = view.get(entity);
    if (auto targetEntity = shadowTreeComponent.targetEntity(registry_)) {
      registry_.get_or_emplace<ComputedShadowTreeComponent>(entity).instantiate(
          registry_, targetEntity.value(), shadowTreeComponent.href(), outWarnings);
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

  instantiateComputedComponents_.publish(registry_, outWarnings);
}

void RenderingContext::instantiateRenderTreeWithPrecomputedTree() {
  registry_.clear<RenderingInstanceComponent>();

  int drawOrder = 0;
  int restorePopDepth = 0;
  ContextPaintServers contextPaintServers;

  std::function<void(Transformd, Entity)> traverseTree = [&](Transformd transform,
                                                             Entity treeEntity) {
    const auto* shadowEntityComponent = registry_.try_get<ShadowEntityComponent>(treeEntity);
    const Entity styleEntity = treeEntity;
    const EntityHandle dataHandle(
        registry_, shadowEntityComponent ? shadowEntityComponent->lightEntity : treeEntity);
    bool traverseChildren = true;
    std::optional<Boxd> clipRect;
    int layerDepth = 0;

    if (const auto* behavior = dataHandle.try_get<RenderingBehaviorComponent>()) {
      if (behavior->behavior == RenderingBehavior::Nonrenderable) {
        return;
      } else if (behavior->behavior == RenderingBehavior::NoTraverseChildren) {
        traverseChildren = false;
      }
    }

    const ComputedStyleComponent& styleComponent =
        registry_.get<ComputedStyleComponent>(styleEntity);
    const auto& properties = styleComponent.properties();

    if (properties.display.getRequired() == Display::None) {
      return;
    }

    if (const auto* sizedElement = dataHandle.try_get<ComputedSizedElementComponent>()) {
      if (sizedElement->bounds.isEmpty()) {
        return;
      }

      transform = sizedElement->computeTransform(dataHandle) * transform;

      if (auto maybeClipRect = sizedElement->clipRect(dataHandle)) {
        ++layerDepth;
        clipRect = maybeClipRect;
      }
    }

    if (const auto* tc = dataHandle.try_get<ComputedTransformComponent>()) {
      transform = tc->transform * transform;
    }

    RenderingInstanceComponent& instance =
        registry_.emplace<RenderingInstanceComponent>(styleEntity);
    instance.drawOrder = drawOrder++;
    instance.restorePopDepth = restorePopDepth;
    instance.transformCanvasSpace = transform;
    instance.clipRect = clipRect;
    instance.dataEntity = dataHandle.entity();

    restorePopDepth = 0;

    // Create a new layer if opacity is less than 1.
    if (properties.opacity.getRequired() < 1.0) {
      instance.isolatedLayer = true;

      // TODO: Calculate hint for size of layer.
      ++layerDepth;
    }

    if (properties.visibility.getRequired() != Visibility::Visible) {
      instance.visible = false;
    }

    if (auto fill = properties.fill.get()) {
      instance.resolvedFill = ResolvePaint(dataHandle, fill.value(), contextPaintServers);
    }

    if (auto stroke = properties.stroke.get()) {
      instance.resolvedStroke = ResolvePaint(dataHandle, stroke.value(), contextPaintServers);
    }

    if (traverseChildren) {
      const TreeComponent& tree = registry_.get<TreeComponent>(treeEntity);
      for (auto cur = tree.firstChild(); cur != entt::null;
           cur = registry_.get<TreeComponent>(cur).nextSibling()) {
        traverseTree(transform, cur);
      }
    }

    if (layerDepth > 0) {
      restorePopDepth += layerDepth;
    }
  };

  const Entity rootEntity = registry_.ctx<DocumentContext>().rootEntity;
  traverseTree(Transformd(), rootEntity);

  registry_.sort<RenderingInstanceComponent>(
      [](const RenderingInstanceComponent& lhs, const RenderingInstanceComponent& rhs) {
        return lhs.drawOrder < rhs.drawOrder;
      });
}

}  // namespace donner::svg
