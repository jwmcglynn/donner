#include "src/svg/components/gradient/gradient_system.h"

#include "src/svg/components/evaluated_reference_component.h"
#include "src/svg/components/gradient/gradient_component.h"
#include "src/svg/components/shadow/computed_shadow_tree_component.h"
#include "src/svg/components/shadow/shadow_tree_component.h"
#include "src/svg/components/style/computed_style_component.h"
#include "src/svg/components/tree_component.h"
#include "src/svg/graph/recursion_guard.h"

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

  const TreeComponent& tree = handle.get<TreeComponent>();
  for (auto cur = tree.firstChild(); cur != entt::null;
       cur = registry.get<TreeComponent>(cur).nextSibling()) {
    // TODO: Detect <desc>, <metadata>, <title> elements.
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

void GradientSystem::instantiateAllComputedComponents(Registry& registry,
                                                      std::vector<ParseError>* outWarnings) {
  // Should instantiate <stop> before gradients.
  for (auto view = registry.view<StopComponent, ComputedStyleComponent>(); auto entity : view) {
    auto [stop, style] = view.get(entity);
    createComputedTypeWithStyle(EntityHandle(registry, entity), stop, style, outWarnings);
  }

  // Create ComputedGradientComponent for all entities in the registry that have a GradientComponent
  for (auto view = registry.view<GradientComponent>(); auto entity : view) {
    std::ignore = registry.emplace_or_replace<ComputedGradientComponent>(entity);
  }

  for (auto view = registry.view<GradientComponent, ComputedStyleComponent>(); auto entity : view) {
    auto [computedGradient, style] = view.get(entity);
    createComputedTypeWithStyle(EntityHandle(registry, entity), computedGradient, style,
                                outWarnings);
  }
}

// Instantiate shadow trees for valid "href" attributes in gradient elements for all elements in the
// registry
void GradientSystem::createGradientShadowTrees(Registry& registry,
                                               std::vector<ParseError>* outWarnings) {
  for (auto view = registry.view<GradientComponent>(); auto entity : view) {
    const auto& [gradient] = view.get(entity);

    if (gradient.href) {
      // Resolve the href to its entity and confirm its a gradient
      if (auto resolvedReference = gradient.href.value().resolve(registry)) {
        const EntityHandle resolvedHandle = resolvedReference.value().handle;
        if (resolvedHandle.all_of<GradientComponent>()) {
          registry.emplace_or_replace<EvaluatedReferenceComponent<GradientComponent>>(
              entity, resolvedHandle);

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

void GradientSystem::initializeComputedGradient(EntityHandle handle,
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
    std::vector<Entity> inheritanceChain;
    inheritanceChain.push_back(handle);

    // If there's an href, first fill the computed component with defaults from that.
    {
      RecursionGuard guard;

      EntityHandle current = handle;
      while (const auto* ref = current.try_get<EvaluatedReferenceComponent<GradientComponent>>()) {
        if (guard.hasRecursion(ref->target)) {
          if (outWarnings) {
            ParseError err;
            err.reason = "Circular gradient inheritance detected";
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

    // Iterate over the inheritance chain backwards to propagate attributes from base -> current.
    EntityHandle base;
    for (auto it = inheritanceChain.rbegin(); it != inheritanceChain.rend(); ++it) {
      EntityHandle cur = EntityHandle(registry, *it);

      auto& curComputed = cur.get_or_emplace<ComputedGradientComponent>();
      initializeComputedGradient(cur, curComputed, outWarnings);

      computedGradient.resolveAndInheritAttributes(cur, base);

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
  const TreeComponent& tree = treeEntity.get<TreeComponent>();
  for (auto cur = tree.firstChild(); cur != entt::null;
       cur = registry.get<TreeComponent>(cur).nextSibling()) {
    if (const auto* stop = registry.try_get<ComputedStopComponent>(cur)) {
      computedGradient.stops.emplace_back(
          GradientStop{stop->properties.offset, stop->properties.stopColor.getRequired(),
                       NarrowToFloat(stop->properties.stopOpacity.getRequired())});
    }
  }
}

const ComputedGradientComponent& GradientSystem::createComputedTypeWithStyle(
    EntityHandle handle, const GradientComponent& gradient, const ComputedStyleComponent& style,
    std::vector<ParseError>* outWarnings) {
  ComputedGradientComponent& computedGradient = handle.get_or_emplace<ComputedGradientComponent>();
  initializeComputedGradient(handle, computedGradient, outWarnings);
  return computedGradient;
}

const ComputedStopComponent& GradientSystem::createComputedTypeWithStyle(
    EntityHandle handle, const StopComponent& stop, const ComputedStyleComponent& style,
    std::vector<ParseError>* outWarnings) {
  return handle.emplace_or_replace<ComputedStopComponent>(
      stop.properties, style, style.properties->unparsedProperties, outWarnings);
}

}  // namespace donner::svg::components
