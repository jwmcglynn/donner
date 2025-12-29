// @file
#include "donner/svg/components/text/TextSystem.h"

#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/components/text/ComputedTextStyleComponent.h"
#include "donner/svg/components/text/TextComponent.h"
#include "donner/svg/components/text/TextPositioningComponent.h"
#include "donner/svg/components/text/TextRootComponent.h"
#include "donner/svg/properties/PresentationAttributeParsing.h"  // IWYU pragma: keep, defines ParsePresentationAttribute
#include "donner/svg/properties/PropertyRegistry.h"

namespace donner::svg::components {

void TextSystem::instantiateAllComputedComponents(Registry& registry,
                                                  std::vector<ParseError>* /*outWarnings*/) {
  auto view = registry.view<TextRootComponent, TextComponent, TextPositioningComponent,
                            ComputedStyleComponent>();
  for (auto entity : view) {
    auto [textComponent, positioningComponent, computedStyle] = view.get(entity);
    auto& computed = registry.get_or_emplace<ComputedTextComponent>(entity);
    auto& textStyle = registry.get_or_emplace<ComputedTextStyleComponent>(entity);

    if (computedStyle.properties) {
      const PropertyRegistry& properties = computedStyle.properties.value();
      textStyle.fontFamily = properties.fontFamily.getRequired();
      textStyle.fontStyle = properties.fontStyle.getRequired();
      textStyle.fontWeight = properties.fontWeight.getRequired();
      textStyle.fontStretch = properties.fontStretch.getRequired();
      textStyle.fontVariant = properties.fontVariant.getRequired();
      textStyle.fontSize = properties.fontSize.getRequired();
      textStyle.letterSpacing = properties.letterSpacing.getRequired();
      textStyle.wordSpacing = properties.wordSpacing.getRequired();
      textStyle.textAnchor = properties.textAnchor.getRequired();
      textStyle.whiteSpace = properties.whiteSpace.getRequired();
      textStyle.direction = properties.direction.getRequired();
    }

    computed.spans.clear();

    auto appendSpan = [&](EntityHandle handle, const TextComponent& text,
                          const TextPositioningComponent& pos,
                          const ComputedTextStyleComponent& spanStyle) {
      ComputedTextComponent::TextSpan span;
      span.text = text.text;
      span.style = spanStyle;
      span.start = 0;
      span.end = static_cast<std::size_t>(text.text.size());

      // Copy all x/y/dx/dy values for per-glyph positioning
      if (!pos.x.empty()) {
        span.x = pos.x;
      } else if (!positioningComponent.x.empty()) {
        span.x = positioningComponent.x;
      }
      if (!pos.y.empty()) {
        span.y = pos.y;
      } else if (!positioningComponent.y.empty()) {
        span.y = positioningComponent.y;
      }
      if (!pos.dx.empty()) {
        span.dx = pos.dx;
      } else if (!positioningComponent.dx.empty()) {
        span.dx = positioningComponent.dx;
      }
      if (!pos.dy.empty()) {
        span.dy = pos.dy;
      } else if (!positioningComponent.dy.empty()) {
        span.dy = positioningComponent.dy;
      }

      if (!pos.rotateDegrees.empty()) {
        span.rotateDegrees = pos.rotateDegrees;
      } else if (!positioningComponent.rotateDegrees.empty()) {
        span.rotateDegrees = positioningComponent.rotateDegrees;
      }

      computed.spans.push_back(std::move(span));
    };

    EntityHandle handle(registry, entity);
    donner::components::ForAllChildrenRecursive(handle, [&](EntityHandle cur) {
      if (!cur.all_of<TextComponent, TextPositioningComponent, ComputedStyleComponent>()) {
        return;
      }

      const auto& text = cur.get<TextComponent>();
      const auto& pos = cur.get<TextPositioningComponent>();
      const auto& childStyle = cur.get<ComputedStyleComponent>();
      auto& computedTextStyle = registry.get_or_emplace<ComputedTextStyleComponent>(cur.entity());

      if (childStyle.properties) {
        const PropertyRegistry& properties = childStyle.properties.value();
        computedTextStyle.fontFamily = properties.fontFamily.getRequired();
        computedTextStyle.fontStyle = properties.fontStyle.getRequired();
        computedTextStyle.fontWeight = properties.fontWeight.getRequired();
        computedTextStyle.fontStretch = properties.fontStretch.getRequired();
        computedTextStyle.fontVariant = properties.fontVariant.getRequired();
        computedTextStyle.fontSize = properties.fontSize.getRequired();
        computedTextStyle.letterSpacing = properties.letterSpacing.getRequired();
        computedTextStyle.wordSpacing = properties.wordSpacing.getRequired();
        computedTextStyle.textAnchor = properties.textAnchor.getRequired();
        computedTextStyle.whiteSpace = properties.whiteSpace.getRequired();
        computedTextStyle.direction = properties.direction.getRequired();
      }

      appendSpan(cur, text, pos, computedTextStyle);
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
