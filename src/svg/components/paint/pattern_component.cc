#include "src/svg/components/paint/pattern_component.h"

#include "src/svg/components/evaluated_reference_component.h"
#include "src/svg/components/tree_component.h"
#include "src/svg/graph/recursion_guard.h"
#include "src/svg/properties/presentation_attribute_parsing.h"  // IWYU pragma: keep, defines ParsePresentationAttribute

namespace donner::svg::components {

void PatternComponent::compute(EntityHandle handle) {
  handle.get_or_emplace<ComputedPatternComponent>().initialize(handle);
}

void ComputedPatternComponent::initialize(EntityHandle handle) {
  if (initialized) {
    return;
  }

  initialized = true;

  Registry& registry = *handle.registry();

  {
    std::vector<Entity> inheritanceChain;
    inheritanceChain.push_back(handle);

    // If there's an href, first fill the computed component with defaults from that.
    {
      RecursionGuard guard;

      EntityHandle current = handle;
      while (const auto* ref = current.try_get<EvaluatedReferenceComponent<PatternComponent>>()) {
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

    // Iterate over the inheritance chain backwards and compute.
    EntityHandle base;
    for (auto it = inheritanceChain.rbegin(); it != inheritanceChain.rend(); ++it) {
      EntityHandle cur = EntityHandle(registry, *it);

      auto& curComputed = cur.get_or_emplace<ComputedPatternComponent>();
      curComputed.initialize(cur);

      inheritAttributes(cur, base);

      base = cur;
    }
  }
}

void ComputedPatternComponent::inheritAttributes(EntityHandle handle, EntityHandle base) {
  if (base) {
    if (auto* computedBase = base.try_get<ComputedPatternComponent>()) {
      patternUnits = computedBase->patternUnits;
      patternContentUnits = computedBase->patternContentUnits;
    }
  }

  // TODO: Inherit viewbox, transform, preserveAspectRatio, x, y, width, and height.

  const PatternComponent& pattern = handle.get<PatternComponent>();
  if (pattern.patternUnits) {
    patternUnits = pattern.patternUnits.value();
  }
  if (pattern.patternContentUnits) {
    patternContentUnits = pattern.patternContentUnits.value();
  }
}

}  // namespace donner::svg::components

namespace donner::svg {

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::Pattern>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  // In SVG2, <pattern> still has normal attributes, not presentation attributes that can be
  // specified in CSS.
  return false;
}

}  // namespace donner::svg
