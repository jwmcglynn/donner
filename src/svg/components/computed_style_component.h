#pragma once

#include "src/svg/components/document_context.h"
#include "src/svg/components/registry.h"
#include "src/svg/components/style_component.h"
#include "src/svg/components/stylesheet_component.h"
#include "src/svg/components/tree_component.h"
#include "src/svg/components/viewbox_component.h"

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

  const Boxd& viewbox() const {
    assert(viewbox_.has_value());
    return viewbox_.value();
  }

  template <ElementWithEntity T>
  void computeProperties(const T& element, Registry& registry, Entity entity) {
    if (properties_) {
      return;  // Already computed.
    }

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
      properties_ = properties.inheritFrom(parentStyleComponent.properties());
      viewbox_ = parentStyleComponent.viewbox_;
    } else {
      properties_ = properties;
    }

    if (auto* viewboxComponent = registry.try_get<ViewboxComponent>(entity);
        viewboxComponent && viewboxComponent->viewbox) {
      viewbox_ = viewboxComponent->viewbox.value();
    } else if (!viewbox_) {
      // If there is no viewbox, default to the size of the canvas.
      const auto& documentContext = registry.ctx<DocumentContext>();
      assert(documentContext.defaultSize.has_value());
      viewbox_ = Boxd(Vector2d::Zero(), documentContext.defaultSize.value());
    }

    // Convert properties to relative transforms.
    // TODO: Set font metrics from properties.
    assert(viewbox_.has_value());
    properties_->resolveUnits(viewbox_.value(), FontMetrics());
  }

private:
  std::optional<svg::PropertyRegistry> properties_;
  std::optional<Boxd> viewbox_;
};

}  // namespace donner
