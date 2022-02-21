#include "src/svg/components/gradient_component.h"

#include "src/base/math_utils.h"
#include "src/svg/components/stop_component.h"
#include "src/svg/components/tree_component.h"

namespace donner::svg {

namespace {

std::vector<GradientStop> FindGradientStops(EntityHandle handle) {
  const Registry& registry = *handle.registry();
  std::vector<GradientStop> stops;

  const TreeComponent& tree = handle.get<TreeComponent>();
  for (auto cur = tree.firstChild(); cur != entt::null;
       cur = registry.get<TreeComponent>(cur).nextSibling()) {
    if (const auto* stop = registry.try_get<ComputedStopComponent>(cur)) {
      stops.emplace_back(GradientStop{stop->properties.offset,
                                      stop->properties.stopColor.getRequired(),
                                      NarrowToFloat(stop->properties.stopOpacity.getRequired())});
    }
  }

  return stops;
}

}  // namespace

void InstantiateGradientComponents(Registry& registry, std::vector<ParseError>* outWarnings) {
  for (auto view = registry.view<GradientComponent>(); auto entity : view) {
    registry.emplace_or_replace<ComputedGradientComponent>(
        entity, FindGradientStops(EntityHandle(registry, entity)));
  }
}

}  // namespace donner::svg
