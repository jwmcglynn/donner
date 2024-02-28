#include "src/svg/components/poly_component.h"

#include "src/svg/properties/presentation_attribute_parsing.h"

namespace donner::svg::components {

void InstantiatePolyComponents(Registry& registry, std::vector<ParseError>* outWarnings) {
  for (auto view = registry.view<PolyComponent, ComputedStyleComponent>(); auto entity : view) {
    auto [component, style] = view.get(entity);
    component.computePathWithPrecomputedStyle(EntityHandle(registry, entity), style, FontMetrics(),
                                              outWarnings);
  }
}

}  // namespace donner::svg::components

namespace donner::svg {

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::Polygon>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  // In SVG2, <polygon> still has normal attributes, not presentation attributes that can be
  // specified in CSS.
  return false;
}

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::Polyline>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  // In SVG2, <polyline> still has normal attributes, not presentation attributes that can be
  // specified in CSS.
  return false;
}

}  // namespace donner::svg
