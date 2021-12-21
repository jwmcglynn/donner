#pragma once

#include <iostream>

#include "src/svg/components/registry.h"
#include "src/svg/components/style_component.h"
#include "src/svg/components/stylesheet_component.h"

namespace donner {

struct ComputedStyleComponent {
  svg::PropertyRegistry properties;

  template <css::ElementLike T>
  void compute(const T& element, Registry& registry, Entity entity) {
    // Apply local style.
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
  }
};

}  // namespace donner
