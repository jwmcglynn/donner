#include "donner/svg/components/paint/PaintSystem.h"

#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/EvaluatedReferenceComponent.h"
#include "donner/svg/components/PreserveAspectRatioComponent.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/layout/ViewBoxComponent.h"
#include "donner/svg/components/paint/GradientComponent.h"
#include "donner/svg/components/paint/MaskComponent.h"
#include "donner/svg/components/paint/PatternComponent.h"
#include "donner/svg/components/shadow/ComputedShadowTreeComponent.h"
#include "donner/svg/components/shadow/ShadowTreeComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/style/StyleSystem.h"
#include "donner/svg/graph/RecursionGuard.h"

namespace donner::svg::components {

namespace {

/**
 * Returns true if the given component does not have any child content other than descriptive
 * elements, per https://www.w3.org/TR/SVG2/pservers.html#PaintServerTemplates
 *
 * @param handle Entity handle to check.
 * @return true if this element has no child content other than structural elements.
 */
bool HasNoStructuralChildren(EntityHandle handle) {
  const Registry& registry = *handle.registry();

  const auto& tree = handle.get<donner::components::TreeComponent>();
  for (auto cur = tree.firstChild(); cur != entt::null;
       cur = registry.get<donner::components::TreeComponent>(cur).nextSibling()) {
    // TODO(jwmcglynn): Detect <desc>, <metadata>, <title> elements.
    return false;
  }

  return true;
}

// Helper to call the callback on each entt::type_list element
template <typename TypeList, typename F, std::size_t... Indices>
constexpr void ForEachShapeImpl(const F& f, std::index_sequence<Indices...>) {
  (f.template operator()<typename entt::type_list_element<Indices, TypeList>::type>(), ...);
}

// Main function to iterate over the tuple
template <typename TypeList, typename F>
constexpr void ForEachShape(const F& f) {
  ForEachShapeImpl<TypeList>(f, std::make_index_sequence<TypeList::size>{});
}

}  // namespace

const ComputedStopComponent& PaintSystem::createComputedStop(EntityHandle handle,
                                                             const StopComponent& stop,
                                                             std::vector<ParseError>* outWarnings) {
  if (const auto* computedStop = handle.try_get<ComputedStopComponent>()) {
    return *computedStop;
  }

  const ComputedStyleComponent& style = StyleSystem().computeStyle(handle, outWarnings);
  return createComputedStopWithStyle(handle, stop, style, outWarnings);
}

void PaintSystem::instantiateAllComputedComponents(Registry& registry,
                                                   std::vector<ParseError>* outWarnings) {
  // Should instantiate <stop> before gradients.
  for (auto view = registry.view<StopComponent, ComputedStyleComponent>(); auto entity : view) {
    auto [stop, style] = view.get(entity);
    createComputedStopWithStyle(EntityHandle(registry, entity), stop, style, outWarnings);
  }

  // Create ComputedGradientComponent for all entities in the registry that have a GradientComponent
  for (auto view = registry.view<GradientComponent>(); auto entity : view) {
    std::ignore = registry.emplace_or_replace<ComputedGradientComponent>(entity);
  }

  for (auto view = registry.view<ComputedGradientComponent>(); auto entity : view) {
    auto [computedGradient] = view.get(entity);
    initializeComputedGradient(EntityHandle(registry, entity), computedGradient, outWarnings);
  }

  // Create ComputedPatternComponent for all entities in the registry that have a PatternComponent
  for (auto view = registry.view<PatternComponent>(); auto entity : view) {
    std::ignore = registry.emplace_or_replace<ComputedPatternComponent>(entity);
  }

  for (auto view = registry.view<ComputedPatternComponent>(); auto entity : view) {
    auto [computedPattern] = view.get(entity);
    initializeComputedPattern(EntityHandle(registry, entity), computedPattern, outWarnings);
  }
}

void PaintSystem::createShadowTrees(Registry& registry, std::vector<ParseError>* outWarnings) {
  createGradientShadowTrees(registry, outWarnings);
  createPatternShadowTrees(registry, outWarnings);
}

void PaintSystem::initializeComputedGradient(EntityHandle handle,
                                             ComputedGradientComponent& computedGradient,
                                             std::vector<ParseError>* outWarnings) {
  if (computedGradient.initialized) {
    return;
  }

  computedGradient.initialized = true;

  Registry& registry = *handle.registry();

  //
  // 1. Inherit attributes following the `href` attribute inheritance chain.
  //
  {
    std::vector<Entity> inheritanceChain = getInheritanceChain(handle, outWarnings);

    // Iterate over the inheritance chain backwards to propagate attributes from base -> current.
    EntityHandle base;
    for (auto it = inheritanceChain.rbegin(); it != inheritanceChain.rend(); ++it) {
      EntityHandle cur = EntityHandle(registry, *it);

      auto& curComputed = cur.get_or_emplace<ComputedGradientComponent>();
      initializeComputedGradient(cur, curComputed, outWarnings);

      computedGradient.inheritAttributesFrom(cur, base);

      base = cur;
    }
  }

  //
  // 2. Find the tree containing the `<stop>` elements by following the shadow tree hierarchy.
  //
  EntityHandle treeEntity = handle;
  {
    RecursionGuard shadowGuard;
    shadowGuard.add(treeEntity);

    while (auto* shadow = treeEntity.try_get<ComputedShadowTreeComponent>()) {
      if (shadow->mainLightRoot() == entt::null) {
        return;
      }

      treeEntity = EntityHandle(registry, shadow->mainLightRoot());

      if (shadowGuard.hasRecursion(treeEntity)) {
        return;
      }

      shadowGuard.add(treeEntity);
    }
  }

  //
  // 3. Parse GradientStop information into the computed component.
  //
  const auto& tree = treeEntity.get<donner::components::TreeComponent>();
  for (auto cur = tree.firstChild(); cur != entt::null;
       cur = registry.get<donner::components::TreeComponent>(cur).nextSibling()) {
    if (const auto* stop = registry.try_get<ComputedStopComponent>(cur)) {
      computedGradient.stops.emplace_back(
          GradientStop{stop->properties.offset, stop->properties.stopColor.getRequired(),
                       NarrowToFloat(stop->properties.stopOpacity.getRequired())});
    }
  }
}

void PaintSystem::initializeComputedPattern(EntityHandle handle,
                                            ComputedPatternComponent& computedPattern,
                                            std::vector<ParseError>* outWarnings) {
  if (computedPattern.initialized) {
    return;
  }

  computedPattern.initialized = true;

  Registry& registry = *handle.registry();

  //
  // 1. Inherit attributes following the `href` attribute inheritance chain.
  //
  std::vector<Entity> inheritanceChain = getInheritanceChain(handle, outWarnings);

  // Iterate over the inheritance chain backwards to propagate attributes from base -> current.
  EntityHandle base;
  for (auto it = inheritanceChain.rbegin(); it != inheritanceChain.rend(); ++it) {
    EntityHandle cur = EntityHandle(registry, *it);

    auto& curComputed = cur.get_or_emplace<ComputedPatternComponent>();
    initializeComputedPattern(cur, curComputed, outWarnings);

    computedPattern.inheritAttributesFrom(cur, base);

    base = cur;
  }

  //
  // 2. Resolve the pattern size attributes
  //
  const auto& style = handle.get<ComputedStyleComponent>();

  // If patternUnits are objectBoundingBox, we want to evaluate percentages to [0, 1]. Otherwise
  // evaluate to userUnits.
  const Boxd tileViewBox = (computedPattern.patternUnits == PatternUnits::ObjectBoundingBox)
                               ? Boxd(Vector2d(), Vector2d(1.0, 1.0))
                               : LayoutSystem().getViewBox(handle);

  computedPattern.tileRect = LayoutSystem().computeSizeProperties(
      handle, computedPattern.sizeProperties, style.properties->unparsedProperties, tileViewBox,
      FontMetrics(), outWarnings);

  //
  // 3. Apply viewBox transform
  //
  // To disambiguate the inherited viewBox, check to see if this pattern has an explicitly-provided
  // viewBox before inheriting from the computed viewBox.
  if (const auto& viewBox = handle.get<ViewBoxComponent>(); viewBox.viewBox) {
    if (const auto* component = handle.try_get<PreserveAspectRatioComponent>()) {
      computedPattern.preserveAspectRatio = component->preserveAspectRatio;
    }
    computedPattern.viewBox = viewBox.viewBox.value();
  }
}

std::vector<Entity> PaintSystem::getInheritanceChain(EntityHandle handle,
                                                     std::vector<ParseError>* outWarnings) {
  std::vector<Entity> inheritanceChain;
  inheritanceChain.push_back(handle);

  // If there's an href, first fill the computed component with defaults from that.
  {
    RecursionGuard guard;

    EntityHandle current = handle;
    while (const auto* ref = current.try_get<EvaluatedReferenceComponent<PaintSystem>>()) {
      if (guard.hasRecursion(ref->target)) {
        if (outWarnings) {
          ParseError err;
          err.reason = "Circular paint inheritance detected";
          outWarnings->push_back(err);
        }

        // Note that in the case of recursion, we simply stop evaluating the inheritance instead
        // of treating the gradient as invalid.
        break;
      }

      guard.add(ref->target);

      inheritanceChain.push_back(ref->target);
      current = ref->target;
    }
  }

  return inheritanceChain;
}

const ComputedStopComponent& PaintSystem::createComputedStopWithStyle(
    EntityHandle handle, const StopComponent& stop, const ComputedStyleComponent& style,
    std::vector<ParseError>* outWarnings) {
  return handle.emplace_or_replace<ComputedStopComponent>(
      stop.properties, style, style.properties->unparsedProperties, outWarnings);
}

// Instantiate shadow trees for valid "href" attributes in gradient elements for all elements in
// the registry
void PaintSystem::createGradientShadowTrees(Registry& registry,
                                            std::vector<ParseError>* outWarnings) {
  for (auto view = registry.view<GradientComponent>(); auto entity : view) {
    const auto& [gradient] = view.get(entity);

    if (gradient.href) {
      // Resolve the href to its entity and confirm its a gradient
      if (auto resolvedReference = gradient.href.value().resolve(registry)) {
        const EntityHandle resolvedHandle = resolvedReference.value().handle;
        if (resolvedHandle.all_of<GradientComponent>()) {
          registry.emplace_or_replace<EvaluatedReferenceComponent<PaintSystem>>(entity,
                                                                                resolvedHandle);

          // If this element has no children, create a shadow tree to clone the `<stop>` elements
          // under this element.
          //
          // From https://www.w3.org/TR/SVG2/pservers.html#PaintServerTemplates
          // > Furthermore, if the current element does not have any child content other than
          // > descriptive elements, than the child content of the template element is cloned to
          // > replace it.
          if (HasNoStructuralChildren(EntityHandle(registry, entity))) {
            // Success: Create the shadow
            registry.get_or_emplace<ShadowTreeComponent>(entity).setMainHref(gradient.href->href);
          }
        } else {
          if (outWarnings) {
            ParseError err;
            err.reason = "Gradient element href=\"" + gradient.href.value().href +
                         "\" attribute points to a non-gradient element, inheritance "
                         "ignored";
            outWarnings->push_back(err);
          }
        }
      }
    }
  }
}

void PaintSystem::createPatternShadowTrees(Registry& registry,
                                           std::vector<ParseError>* outWarnings) {
  for (auto view = registry.view<PatternComponent>(); auto entity : view) {
    const auto& [pattern] = view.get(entity);

    if (pattern.href) {
      if (auto resolvedReference = pattern.href.value().resolve(registry)) {
        const EntityHandle resolvedHandle = resolvedReference.value().handle;
        if (resolvedHandle.all_of<PatternComponent>()) {
          registry.emplace_or_replace<EvaluatedReferenceComponent<PaintSystem>>(entity,
                                                                                resolvedHandle);

          if (HasNoStructuralChildren(EntityHandle(registry, entity))) {
            registry.get_or_emplace<ShadowTreeComponent>(entity).setMainHref(pattern.href->href);
          }
        } else {
          if (outWarnings) {
            ParseError err;
            err.reason = "Pattern element href=\"" + pattern.href.value().href +
                         "\" attribute points to a non-gradient element, inheritance "
                         "ignored";
            outWarnings->push_back(err);
          }
        }
      }
    }
  }
}

}  // namespace donner::svg::components
