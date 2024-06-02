#include "src/svg/components/filter/filter_system.h"

#include "src/svg/components/filter/filter_primitive_component.h"
#include "src/svg/components/tree_component.h"

namespace donner::svg::components {

void FilterSystem::createComputedFilter(EntityHandle handle, const FilterComponent& component,
                                        std::vector<ParseError>* outWarnings) {
  const Registry& registry = *handle.registry();

  std::vector<FilterEffect> effectChain;

  // Find all FilterPrimitiveComponent instances in this filter
  const TreeComponent& tree = handle.get<TreeComponent>();
  for (auto cur = tree.firstChild(); cur != entt::null;
       cur = registry.get<TreeComponent>(cur).nextSibling()) {
    if (const auto* primitive = registry.try_get<FilterPrimitiveComponent>(cur)) {
      // Determine which filter primitive we have.
      if (auto* blur = registry.try_get<FEGaussianBlurComponent>(cur)) {
        effectChain.emplace_back(FilterEffect::Blur{
            .stdDeviationX = Lengthd(blur->stdDeviationX),
            .stdDeviationY = Lengthd(blur->stdDeviationY),
        });
      }
    }
  }

  if (!effectChain.empty()) {
    ComputedFilterComponent& computed = handle.emplace_or_replace<ComputedFilterComponent>();
    computed.effectChain = std::move(effectChain);
  } else {
    handle.remove<ComputedFilterComponent>();
  }
}

void FilterSystem::instantiateAllComputedComponents(Registry& registry,
                                                    std::vector<ParseError>* outWarnings) {
  for (auto entity : registry.view<FilterComponent>()) {
    createComputedFilter(EntityHandle(registry, entity), registry.get<FilterComponent>(entity),
                         outWarnings);
  }
}

}  // namespace donner::svg::components
