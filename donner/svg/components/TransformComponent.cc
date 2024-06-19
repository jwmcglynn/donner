#include "donner/svg/components/TransformComponent.h"

#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/parser/CssTransformParser.h"
#include "donner/svg/parser/TransformParser.h"

namespace donner::svg::components {

void TransformComponent::computeWithPrecomputedStyle(EntityHandle handle,
                                                     const ComputedStyleComponent& style,
                                                     const FontMetrics& fontMetrics,
                                                     std::vector<parser::ParseError>* outWarnings) {
  // TODO: This should avoid recomputing the transform each request.
  const auto& properties = style.properties->unparsedProperties;
  if (auto it = properties.find("transform"); it != properties.end()) {
    const parser::UnparsedProperty& property = it->second;

    parser::PropertyParseFnParams params;
    params.valueOrComponents = property.declaration.values;
    params.specificity = property.specificity;
    params.parseBehavior = parser::PropertyParseBehavior::AllowUserUnits;

    auto maybeError = Parse(
        params,
        [](const parser::PropertyParseFnParams& params) {
          if (const std::string_view* str =
                  std::get_if<std::string_view>(&params.valueOrComponents)) {
            return parser::TransformParser::Parse(*str).map<CssTransform>(
                [](const Transformd& transform) { return CssTransform(transform); });
          } else {
            return parser::CssTransformParser::Parse(params.components());
          }
        },
        &transform);

    if (maybeError && outWarnings) {
      outWarnings->emplace_back(std::move(maybeError.value()));
    }
  }

  auto& computedTransform = handle.get_or_emplace<ComputedTransformComponent>();
  if (transform.get()) {
    computedTransform.rawCssTransform = transform.get().value();
    computedTransform.transform =
        transform.get().value().compute(style.viewbox.value(), fontMetrics);
  } else {
    computedTransform.transform = Transformd();
  }
}

void ComputeAllTransforms(Registry& registry, std::vector<parser::ParseError>* outWarnings) {
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

}  // namespace donner::svg::components
