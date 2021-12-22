#pragma once

#include <iostream>

#include "src/svg/components/registry.h"
#include "src/svg/components/style_component.h"
#include "src/svg/components/stylesheet_component.h"
#include "src/svg/components/tree_component.h"

namespace donner {

template <typename T>
concept ElementWithEntity = requires(T t) {
  css::ElementLike<T>;
  { t.entity() } -> std::same_as<Entity>;
};

struct ComputedStyleComponent {
  ComputedStyleComponent() {}

  const svg::PropertyRegistry& properties() const {
    assert(properties_);
    return properties_.value();
  }

  template <ElementWithEntity T>
  void computeProperties(const T& element, Registry& registry, Entity entity) {
    if (!properties_) {
      properties_ = compute(element, registry, entity);
    }
  }

private:
  template <ElementWithEntity T>
  static svg::PropertyRegistry compute(const T& element, Registry& registry, Entity entity) {
    // Apply local style.
    svg::PropertyRegistry properties;
    if (auto* styleComponent = registry.try_get<StyleComponent>(entity)) {
      properties = styleComponent->properties;
    } else {
      properties = svg::PropertyRegistry();
    }

    // Apply style from stylesheets.
    for (auto view = registry.view<StylesheetComponent>(); auto entity : view) {
      auto [stylesheet] = view.get(entity);

      for (const css::SelectorRule& rule : stylesheet.stylesheet.rules()) {
        if (css::SelectorMatchResult match = rule.selector.matches(element); match) {
          for (const auto& declaration : rule.declarations) {
            properties.parseProperty(declaration, match.specificity);
          }
        }
      }
    }

    // Inherit from parent.
    if (auto maybeParent = element.parentElement()) {
      const T& parent = maybeParent.value();
      ComputedStyleComponent& parentStyleComponent =
          registry.get_or_emplace<ComputedStyleComponent>(parent.entity());
      parentStyleComponent.computeProperties(parent, registry, parent.entity());
      return properties.inheritFrom(parentStyleComponent.properties());
    } else {
      return properties;
    }
  }

  std::optional<svg::PropertyRegistry> properties_;
};

}  // namespace donner
