// @file
#include "donner/svg/components/text/TextSystem.h"

#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/components/text/TextComponent.h"
#include "donner/svg/components/text/TextPositioningComponent.h"
#include "donner/svg/components/text/TextRootComponent.h"
#include "donner/svg/properties/PresentationAttributeParsing.h"  // IWYU pragma: keep, defines ParsePresentationAttribute

namespace donner::svg::components {

void TextSystem::instantiateAllComputedComponents(Registry& registry,
                                                  std::vector<ParseError>* /*outWarnings*/) {
  auto view = registry.view<TextRootComponent, TextComponent, TextPositioningComponent>();
  for (auto entity : view) {
    auto [textComponent, positioningComponent] = view.get(entity);
    auto& computed = registry.get_or_emplace<ComputedTextComponent>(entity);

    computed.spans.clear();

    auto appendSpan = [&](EntityHandle handle, const TextComponent& text,
                          const TextPositioningComponent& pos) {
      ComputedTextComponent::TextSpan span;
      span.text = text.text;
      span.start = 0;
      span.end = static_cast<std::size_t>(text.text.size());

      if (!pos.x.empty()) {
        span.x = pos.x[0];
      } else if (!positioningComponent.x.empty()) {
        span.x = positioningComponent.x[0];
      }
      if (!pos.y.empty()) {
        span.y = pos.y[0];
      } else if (!positioningComponent.y.empty()) {
        span.y = positioningComponent.y[0];
      }
      if (!pos.dx.empty()) {
        span.dx = pos.dx[0];
      } else if (!positioningComponent.dx.empty()) {
        span.dx = positioningComponent.dx[0];
      }
      if (!pos.dy.empty()) {
        span.dy = pos.dy[0];
      } else if (!positioningComponent.dy.empty()) {
        span.dy = positioningComponent.dy[0];
      }

      if (!pos.rotateDegrees.empty()) {
        span.rotateDegrees = pos.rotateDegrees[0];
      } else if (!positioningComponent.rotateDegrees.empty()) {
        span.rotateDegrees = positioningComponent.rotateDegrees[0];
      }

      computed.spans.push_back(std::move(span));
    };

    EntityHandle handle(registry, entity);
    donner::components::ForAllChildrenRecursive(handle, [&](EntityHandle cur) {
      if (!cur.all_of<TextComponent, TextPositioningComponent>()) {
        return;
      }

      appendSpan(cur, cur.get<TextComponent>(), cur.get<TextPositioningComponent>());
    });
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
