#include "donner/svg/components/RenderingContext.h"

#include "donner/svg/components/DocumentContext.h"
#include "donner/svg/components/IdComponent.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/TransformComponent.h"
#include "donner/svg/components/filter/FilterComponent.h"
#include "donner/svg/components/filter/FilterSystem.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/layout/SizedElementComponent.h"
#include "donner/svg/components/paint/GradientComponent.h"
#include "donner/svg/components/paint/PaintSystem.h"
#include "donner/svg/components/paint/PatternComponent.h"
#include "donner/svg/components/shadow/ComputedShadowTreeComponent.h"
#include "donner/svg/components/shadow/OffscreenShadowTreeComponent.h"
#include "donner/svg/components/shadow/ShadowBranch.h"
#include "donner/svg/components/shadow/ShadowTreeComponent.h"
#include "donner/svg/components/shadow/ShadowTreeSystem.h"
#include "donner/svg/components/shape/ComputedPathComponent.h"
#include "donner/svg/components/shape/ShapeSystem.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/style/StyleSystem.h"

namespace donner::svg::components {

namespace {

/**
 * The current value of the context-fill and context-stroke paint servers, based on the rules
 * described here: https://www.w3.org/TR/SVG2/painting.html#SpecifyingPaint
 */
struct ContextPaintServers {
  ResolvedPaintServer contextFill = PaintServer::None();
  ResolvedPaintServer contextStroke = PaintServer::None();
};

bool IsValidPaintServer(EntityHandle handle) {
  return handle.any_of<ComputedGradientComponent, ComputedPatternComponent>();
}

class RenderingContextImpl {
public:
  explicit RenderingContextImpl(Registry& registry, bool verbose)
      : registry_(registry), verbose_(verbose) {}

  /**
   * Traverse a tree, instantiating each entity in the tree.
   *
   * @param transform The parent transform to apply to the entity.
   * @param treeEntity Current entity in the tree or shadow tree.
   * @param lastRenderedEntityIfSubtree Optional, entity of the last rendered element if this is a
   *   subtree.
   * @return The last rendered entity.
   */
  // TODO: Since 'stroke' and 'fill' may reference the same tree, we need to create two instances of
  // it in the render tree.
  void traverseTree(Transformd transform, Entity treeEntity,
                    Entity* lastRenderedEntityIfSubtree = nullptr) {
    const auto* shadowEntityComponent = registry_.try_get<ShadowEntityComponent>(treeEntity);
    const Entity styleEntity = treeEntity;
    const EntityHandle dataHandle(
        registry_, shadowEntityComponent ? shadowEntityComponent->lightEntity : treeEntity);
    bool traverseChildren = true;
    std::optional<Boxd> clipRect;
    int layerDepth = 0;
    std::optional<ContextPaintServers> savedContextPaintServers;
    bool appliesTransform = true;

    if (const auto* behavior = dataHandle.try_get<RenderingBehaviorComponent>()) {
      if (behavior->behavior == RenderingBehavior::Nonrenderable) {
        return;
      } else if (behavior->behavior == RenderingBehavior::NoTraverseChildren) {
        traverseChildren = false;
      } else if (behavior->behavior == RenderingBehavior::ShadowOnlyChildren) {
        if (!shadowEntityComponent) {
          traverseChildren = false;
        }
      }

      appliesTransform = behavior->appliesTransform;
    }

    const ComputedStyleComponent& styleComponent =
        registry_.get<ComputedStyleComponent>(styleEntity);
    const auto& properties = styleComponent.properties.value();

    if (properties.display.getRequired() == Display::None) {
      return;
    }

    if (const auto* sizedElement = dataHandle.try_get<ComputedSizedElementComponent>()) {
      if (sizedElement->bounds.isEmpty()) {
        return;
      }

      transform = LayoutSystem().computeTransform(dataHandle, *sizedElement) * transform;

      if (auto maybeClipRect = LayoutSystem().clipRect(dataHandle)) {
        ++layerDepth;
        clipRect = maybeClipRect;
      }
    }

    if (const auto* tc = dataHandle.try_get<ComputedTransformComponent>(); tc && appliesTransform) {
      transform = tc->transform * transform;
    }

    RenderingInstanceComponent& instance =
        registry_.emplace<RenderingInstanceComponent>(styleEntity);
    instance.drawOrder = drawOrder_++;
    instance.transformCanvasSpace = transform;
    instance.clipRect = clipRect;
    instance.dataEntity = dataHandle.entity();

    if (verbose_) {
      std::cout << "Instantiating " << TypeToString(dataHandle.get<TreeComponent>().type()) << " ";

      if (const auto* idComponent = dataHandle.try_get<IdComponent>()) {
        std::cout << "id=" << idComponent->id << " ";
      }

      std::cout << dataHandle.entity();
      if (instance.isShadow(registry_)) {
        std::cout << " (shadow " << styleEntity << ")";
      }

      std::cout << "\n";
    }

    const bool hasFilterEffect = !properties.filter.getRequired().is<FilterEffect::None>();

    // Create a new layer if opacity is less than 1.
    if (properties.opacity.getRequired() < 1.0 || hasFilterEffect) {
      instance.isolatedLayer = true;

      // TODO: Calculate hint for size of layer.
      ++layerDepth;
    }

    if (properties.visibility.getRequired() != Visibility::Visible) {
      instance.visible = false;
    }

    if (hasFilterEffect) {
      instance.resolvedFilter = resolveFilter(dataHandle, properties.filter.getRequired());
    }

    const ShadowTreeComponent* shadowTree = dataHandle.try_get<ShadowTreeComponent>();
    const bool setsContextColors = shadowTree && shadowTree->setsContextColors;

    if (setsContextColors || (instance.visible && dataHandle.all_of<ComputedPathComponent>())) {
      if (auto fill = properties.fill.get()) {
        instance.resolvedFill = resolvePaint(ShadowBranchType::OffscreenFill, dataHandle,
                                             fill.value(), contextPaintServers_);
      }

      if (auto stroke = properties.stroke.get()) {
        instance.resolvedStroke = resolvePaint(ShadowBranchType::OffscreenStroke, dataHandle,
                                               stroke.value(), contextPaintServers_);
      }

      // Save the current context paint servers if this is a shadow tree host.
      if (setsContextColors) {
        savedContextPaintServers = contextPaintServers_;
        contextPaintServers_.contextFill = instance.resolvedFill;
        contextPaintServers_.contextStroke = instance.resolvedStroke;
      }
    }

    lastRenderedEntity_ = styleEntity;

    if (traverseChildren) {
      const TreeComponent& tree = registry_.get<TreeComponent>(treeEntity);
      for (auto cur = tree.firstChild(); cur != entt::null;
           cur = registry_.get<TreeComponent>(cur).nextSibling()) {
        traverseTree(transform, cur);
      }
    }

    if (layerDepth > 0) {
      instance.subtreeInfo = SubtreeInfo{lastRenderedEntity_, layerDepth};
    }

    if (savedContextPaintServers) {
      contextPaintServers_ = savedContextPaintServers.value();
    }

    if (lastRenderedEntityIfSubtree) {
      *lastRenderedEntityIfSubtree = lastRenderedEntity_;
    }
  }

  std::optional<SubtreeInfo> instantiateOffscreenSubtree(EntityHandle shadowHostHandle,
                                                         ShadowBranchType branchType) {
    const auto* computedShadowTree = shadowHostHandle.try_get<ComputedShadowTreeComponent>();
    if (!computedShadowTree) {
      // If there's not a shadow tree, there is no offscreen subtree.  This is a gradient and not a
      // <pattern>.
      return std::nullopt;
    }

    const std::optional<size_t> maybeShadowIndex =
        computedShadowTree->findOffscreenShadow(branchType);
    if (!maybeShadowIndex) {
      // Theres no subtree here.
      return std::nullopt;
    }

    Entity lastEntity = entt::null;
    traverseTree(Transformd(), computedShadowTree->offscreenShadowRoot(maybeShadowIndex.value()),
                 &lastEntity);

    if (lastEntity != entt::null) {
      return SubtreeInfo{lastEntity, 0};
    } else {
      // This could happen if the subtree has no nodes.
      return std::nullopt;
    }
  }

  ResolvedPaintServer resolvePaint(ShadowBranchType branchType, EntityHandle dataHandle,
                                   const PaintServer& paint,
                                   const ContextPaintServers& contextPaintServers) {
    if (paint.is<PaintServer::Solid>()) {
      return paint.get<PaintServer::Solid>();
    } else if (paint.is<PaintServer::ElementReference>()) {
      const PaintServer::ElementReference& ref = paint.get<PaintServer::ElementReference>();

      // Only resolve paints if the paint server references a supported <pattern> or gradient
      // element, and the shadow tree was instantiated. If the shadow tree is not instantiated, that
      // indicates there was recursion and we treat the reference as invalid.
      if (auto resolvedRef = ref.reference.resolve(*dataHandle.registry());
          resolvedRef && IsValidPaintServer(resolvedRef->handle)) {
        return PaintResolvedReference{resolvedRef.value(), ref.fallback,
                                      instantiateOffscreenSubtree(dataHandle, branchType)};
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

  ResolvedFilterEffect resolveFilter(EntityHandle dataHandle, const FilterEffect& filter) {
    if (filter.is<FilterEffect::ElementReference>()) {
      const FilterEffect::ElementReference& ref = filter.get<FilterEffect::ElementReference>();

      if (auto resolvedRef = ref.reference.resolve(*dataHandle.registry());
          resolvedRef && resolvedRef->handle.all_of<ComputedFilterComponent>()) {
        return resolvedRef.value();
      } else {
        // Empty result.
        return std::vector<FilterEffect>();
      }
    } else {
      std::vector<FilterEffect> result;
      result.push_back(filter);
      return result;
    }
  }

private:
  Registry& registry_;
  bool verbose_;

  int drawOrder_ = 0;
  Entity lastRenderedEntity_ = entt::null;
  ContextPaintServers contextPaintServers_;
};

void InstantiatePaintShadowTree(Registry& registry, Entity entity, ShadowBranchType branchType,
                                const PaintServer& paint,
                                std::vector<parser::ParseError>* outWarnings) {
  if (paint.is<PaintServer::ElementReference>()) {
    const PaintServer::ElementReference& ref = paint.get<PaintServer::ElementReference>();

    if (auto resolvedRef = ref.reference.resolve(registry);
        resolvedRef && resolvedRef->handle.all_of<PatternComponent>()) {
      auto& offscreenShadowComponent =
          registry.get_or_emplace<OffscreenShadowTreeComponent>(entity);
      offscreenShadowComponent.setBranchHref(branchType, ref.reference.href);
    }
  }
}

}  // namespace

RenderingContext::RenderingContext(Registry& registry) : registry_(registry) {}

void RenderingContext::instantiateRenderTree(bool verbose,
                                             std::vector<parser::ParseError>* outWarnings) {
  createComputedComponents(outWarnings);
  instantiateRenderTreeWithPrecomputedTree(verbose);
}

// 1. Setup shadow trees
// 2. Evaluate and propagate styles
// 3. Instantiate shadow trees and propagate style information to them
// 4. Determine element sizes and layout
// 5. Compute transforms
// 6. Decompose shapes to paths
// 7. Resolve fill and stroke references (paints)
// 8. Resolve filter references
void RenderingContext::createComputedComponents(std::vector<parser::ParseError>* outWarnings) {
  // Evaluate conditional components which may create shadow trees.
  PaintSystem().createShadowTrees(registry_, outWarnings);

  // Instantiate shadow trees.
  for (auto view = registry_.view<ShadowTreeComponent>(); auto entity : view) {
    auto [shadowTreeComponent] = view.get(entity);
    if (auto targetEntity = shadowTreeComponent.mainTargetEntity(registry_)) {
      auto& shadow = registry_.get_or_emplace<ComputedShadowTreeComponent>(entity);
      ShadowTreeSystem().populateInstance(EntityHandle(registry_, entity), shadow,
                                          ShadowBranchType::Main, targetEntity.value(),
                                          shadowTreeComponent.mainHref().value(), outWarnings);

    } else if (shadowTreeComponent.mainHref() && outWarnings) {
      // We had a main href but it failed to resolve.
      parser::ParseError err;
      err.reason = std::string("Warning: Failed to resolve shadow tree target with href '") +
                   shadowTreeComponent.mainHref().value_or("") + "'";
      outWarnings->emplace_back(std::move(err));
    }
  }

  StyleSystem().computeAllStyles(registry_, outWarnings);

  // Instantiate shadow trees for 'fill' and 'stroke' referencing a <pattern>. This needs to occur
  // after those styles are evaluated, and after which we need to compute the styles for that subset
  // of the tree.
  for (auto view = registry_.view<ComputedStyleComponent>(); auto entity : view) {
    auto [styleComponent] = view.get(entity);

    const auto& properties = styleComponent.properties.value();

    if (auto fill = properties.fill.get()) {
      InstantiatePaintShadowTree(registry_, entity, ShadowBranchType::OffscreenFill, fill.value(),
                                 outWarnings);
    }

    if (auto stroke = properties.stroke.get()) {
      InstantiatePaintShadowTree(registry_, entity, ShadowBranchType::OffscreenStroke,
                                 stroke.value(), outWarnings);
    }
  }

  for (auto view = registry_.view<OffscreenShadowTreeComponent>(); auto entity : view) {
    auto [offscreenTree] = view.get(entity);
    for (auto [branchType, ref] : offscreenTree.branches()) {
      if (auto targetEntity = offscreenTree.branchTargetEntity(registry_, branchType)) {
        auto& computedShadow = registry_.get_or_emplace<ComputedShadowTreeComponent>(entity);

        const std::optional<size_t> maybeInstanceIndex = ShadowTreeSystem().populateInstance(
            EntityHandle(registry_, entity), computedShadow, branchType, targetEntity.value(),
            ref.href, outWarnings);

        if (maybeInstanceIndex) {
          // Apply styles to the tree.
          const std::span<const Entity> shadowEntities =
              computedShadow.offscreenShadowEntities(maybeInstanceIndex.value());
          StyleSystem().computeStylesFor(registry_, shadowEntities, outWarnings);
        }
      } else if (outWarnings) {
        // We had a href but it failed to resolve.
        parser::ParseError err;
        err.reason =
            std::string("Warning: Failed to resolve offscreen shadow tree target with href '") +
            ref.href + "'";
        outWarnings->emplace_back(std::move(err));
      }
    }
  }

  LayoutSystem().instantiateAllComputedComponents(registry_, outWarnings);

  ComputeAllTransforms(registry_, outWarnings);

  ShapeSystem().instantiateAllComputedPaths(registry_, outWarnings);

  PaintSystem().instantiateAllComputedComponents(registry_, outWarnings);

  FilterSystem().instantiateAllComputedComponents(registry_, outWarnings);
}

void RenderingContext::instantiateRenderTreeWithPrecomputedTree(bool verbose) {
  registry_.clear<RenderingInstanceComponent>();

  const Entity rootEntity = registry_.ctx().get<DocumentContext>().rootEntity;

  RenderingContextImpl impl(registry_, verbose);
  impl.traverseTree(Transformd(), rootEntity);

  registry_.sort<RenderingInstanceComponent>(
      [](const RenderingInstanceComponent& lhs, const RenderingInstanceComponent& rhs) {
        return lhs.drawOrder < rhs.drawOrder;
      });
}

}  // namespace donner::svg::components