#include "src/svg/components/line_component.h"

#include "src/svg/properties/presentation_attribute_parsing.h"

namespace donner::svg {

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::Line>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  // In SVG2, <line> still has normal attributes, not presentation attributes that can be specified
  // in CSS.
  return false;
}

void InstantiateLineComponents(Registry& registry, std::vector<ParseError>* outWarnings) {
  for (auto view = registry.view<LineComponent, ComputedStyleComponent>(); auto entity : view) {
    auto [component, style] = view.get(entity);
    component.computePathWithPrecomputedStyle(EntityHandle(registry, entity), style, FontMetrics(),
                                              outWarnings);
  }
}

}  // namespace donner::svg
