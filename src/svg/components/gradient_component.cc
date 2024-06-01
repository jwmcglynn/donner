#include "src/svg/components/gradient_component.h"

#include "src/base/math_utils.h"
#include "src/svg/components/computed_shadow_tree_component.h"
#include "src/svg/components/evaluated_reference_component.h"
#include "src/svg/components/linear_gradient_component.h"
#include "src/svg/components/radial_gradient_component.h"
#include "src/svg/components/shadow_tree_component.h"
#include "src/svg/components/stop_component.h"
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

}  // namespace

void GradientComponent::compute(EntityHandle handle) {
  handle.get_or_emplace<ComputedGradientComponent>().initialize(handle);
}

void ComputedGradientComponent::initialize(EntityHandle handle) {
  if (initialized) {
    return;
  }

  initialized = true;

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
          // TODO: Propagate warning.
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
      curComputed.initialize(cur);

      resolveAndInheritAttributes(cur, base);

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
      stops.emplace_back(GradientStop{stop->properties.offset,
                                      stop->properties.stopColor.getRequired(),
                                      NarrowToFloat(stop->properties.stopOpacity.getRequired())});
    }
  }
}

// Resolve unspecified attributes to default values or inherit from the given base gradient element.
// This method is used to propagate attributes such as `x1`, `y1`, `cx`, `cy`, `r`, etc from the
// base element to the current element.
void ComputedGradientComponent::resolveAndInheritAttributes(EntityHandle handle,
                                                            EntityHandle base) {
  if (base) {
    if (auto* computedBase = base.try_get<ComputedGradientComponent>()) {
      gradientUnits = computedBase->gradientUnits;
      spreadMethod = computedBase->spreadMethod;
    }
  }

  // This lets <linearGradient> and <radialGradient> elements inherit shared attributes from each
  // other
  const GradientComponent& gradient = handle.get<GradientComponent>();
  if (gradient.gradientUnits) {
    gradientUnits = gradient.gradientUnits.value();
  }
  if (gradient.spreadMethod) {
    spreadMethod = gradient.spreadMethod.value();
  }

  // Inherit attributes from matching element types
  if (auto* linearGradient = handle.try_get<LinearGradientComponent>()) {
    linearGradient->inheritAttributes(handle, base);
  }

  if (auto* radialGradient = handle.try_get<RadialGradientComponent>()) {
    radialGradient->inheritAttributes(handle, base);
  }
}

// Instantiate shadow trees for valid "href" attributes in gradient elements for all elements in the
// registry
void EvaluateConditionalGradientShadowTrees(Registry& registry,
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

// Create ComputedGradientComponent for all entities in the registry that have a GradientComponent
void InstantiateGradientComponents(Registry& registry, std::vector<ParseError>* outWarnings) {
  for (auto view = registry.view<GradientComponent>(); auto entity : view) {
    std::ignore = registry.emplace_or_replace<ComputedGradientComponent>(entity);
  }

  for (auto view = registry.view<ComputedGradientComponent>(); auto entity : view) {
    auto [computedGradient] = view.get(entity);
    computedGradient.initialize(EntityHandle(registry, entity));
  }
}

}  // namespace donner::svg::components
