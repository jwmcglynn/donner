// @file
#include "donner/svg/components/text/TextSystem.h"

#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/components/text/TextComponent.h"
#include "donner/svg/components/text/TextPositioningComponent.h"
#include "donner/svg/properties/PresentationAttributeParsing.h"  // IWYU pragma: keep, defines ParsePresentationAttribute

namespace donner::svg::components {

void TextSystem::instantiateAllComputedComponents(Registry& registry,
                                                  std::vector<ParseError>* /*outWarnings*/) {
  auto view = registry.view<TextComponent, TextPositioningComponent>();
  for (auto entity : view) {
    auto [textComponent, positioningComponent] = view.get(entity);
    auto& computed = registry.get_or_emplace<ComputedTextComponent>(entity);

    computed.spans.clear();

    // Single span for now: entire text
    ComputedTextComponent::TextSpan span;
    span.text = textComponent.text;
    span.start = 0;
    span.end = static_cast<std::size_t>(textComponent.text.size());

    // Apply first positions if available
    if (!positioningComponent.x.empty()) {
      span.x = positioningComponent.x[0];
    }
    if (!positioningComponent.y.empty()) {
      span.y = positioningComponent.y[0];
    }
    if (!positioningComponent.dx.empty()) {
      span.dx = positioningComponent.dx[0];
    }
    if (!positioningComponent.dy.empty()) {
      span.dy = positioningComponent.dy[0];
    }

    if (!positioningComponent.rotateDegrees.empty()) {
      // store rotation as a length (degrees)
      span.rotate = Lengthd(positioningComponent.rotateDegrees[0], Lengthd::Unit::None);
    }

    computed.spans.push_back(std::move(span));
  }
}

}  // namespace donner::svg::components

namespace donner::svg::parser {

// SVGTextElement shares this component.
template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::Text>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  // TODO: Determine if <text> has presentation attributes
  return false;
}

// SVGTSpanElement shares this component.
template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::TSpan>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  // TODO: Determine if <tspan> has presentation attributes
  return false;
}

}  // namespace donner::svg::parser
