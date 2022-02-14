#include "src/svg/components/transform_component.h"

#include "src/svg/components/computed_style_component.h"
#include "src/svg/parser/css_transform_parser.h"
#include "src/svg/parser/transform_parser.h"

namespace donner::svg {

void TransformComponent::computeWithPrecomputedStyle(EntityHandle handle,
                                                     const ComputedStyleComponent& style,
                                                     const FontMetrics& fontMetrics,
                                                     std::vector<ParseError>* outWarnings) {
  const auto& properties = style.properties().unparsedProperties;
  if (auto it = properties.find("transform"); it != properties.end()) {
    const UnparsedProperty& property = it->second;

    PropertyParseFnParams params;
    params.valueOrComponents = property.declaration.values;
    params.specificity = property.specificity;
    params.allowUserUnits = false;

    auto maybeError = Parse(
        params,
        [](const PropertyParseFnParams& params) {
          if (const std::string_view* str =
                  std::get_if<std::string_view>(&params.valueOrComponents)) {
            return TransformParser::Parse(*str).map<CssTransform>(
                [](const Transformd& transform) { return CssTransform(transform); });
          } else {
            return CssTransformParser::Parse(params.components());
          }
        },
        &transform);

    if (maybeError && outWarnings) {
      outWarnings->emplace_back(std::move(maybeError.value()));
    }
  }

  auto& computedTransform = handle.get_or_emplace<ComputedTransformComponent>().transform;
  if (transform.get()) {
    computedTransform = transform.get().value().compute(style.viewbox(), fontMetrics);
  } else {
    computedTransform = Transformd();
  }
}

void TransformComponent::compute(EntityHandle handle, const FontMetrics& fontMetrics) {
  ComputedStyleComponent& style = handle.get_or_emplace<ComputedStyleComponent>();
  style.computeProperties(handle);

  return computeWithPrecomputedStyle(handle, style, fontMetrics, nullptr);
}

void ComputeAllTransforms(Registry& registry, std::vector<ParseError>* outWarnings) {
  // Create placeholder ComputedTransformComponents for all elements in the tree.
  for (auto entity : registry.view<TransformComponent>()) {
    std::ignore = registry.get_or_emplace<ComputedTransformComponent>(entity);
    std::ignore = registry.get_or_emplace<ComputedStyleComponent>(entity);
  }

  for (auto view = registry.view<TransformComponent, ComputedStyleComponent>();
       auto entity : view) {
    auto [transform, style] = view.get(entity);
    transform.computeWithPrecomputedStyle(EntityHandle(registry, entity), style, FontMetrics(),
                                          outWarnings);
  }
}

}  // namespace donner::svg
