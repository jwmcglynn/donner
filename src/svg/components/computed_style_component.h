#pragma once

#include <functional>
#include <tuple>

#include "src/svg/components/class_component.h"
#include "src/svg/components/document_context.h"
#include "src/svg/components/registry.h"
#include "src/svg/components/shadow_entity_component.h"
#include "src/svg/components/style_component.h"
#include "src/svg/components/stylesheet_component.h"
#include "src/svg/components/tree_component.h"
#include "src/svg/components/viewbox_component.h"

namespace donner::svg {

struct ShadowedElementAdapter {
  ShadowedElementAdapter(Registry& registry, Entity treeEntity, Entity dataEntity)
      : registry_(registry), treeEntity_(treeEntity), dataEntity_(dataEntity) {}

  Entity entity() const { return treeEntity_; }

  std::optional<ShadowedElementAdapter> parentElement() const {
    const Entity target = registry_.get().get<TreeComponent>(treeEntity_).parent();
    return target != entt::null ? std::make_optional(create(target)) : std::nullopt;
  }

  std::optional<ShadowedElementAdapter> previousSibling() const {
    const Entity target = registry_.get().get<TreeComponent>(treeEntity_).previousSibling();
    return target != entt::null ? std::make_optional(create(target)) : std::nullopt;
  }

  RcString typeString() const {
    return registry_.get().get<TreeComponent>(treeEntity_).typeString();
  }

  RcString id() const {
    if (const auto* component = registry_.get().try_get<IdComponent>(dataEntity_)) {
      return component->id;
    } else {
      return "";
    }
  }

  RcString className() const {
    if (const auto* component = registry_.get().try_get<ClassComponent>(dataEntity_)) {
      return component->className;
    } else {
      return "";
    }
  }

  bool hasAttribute(std::string_view name) const {
    // TODO
    return false;
  }

  std::optional<RcString> getAttribute(std::string_view name) const {
    // TODO
    return std::nullopt;
  }

private:
  ShadowedElementAdapter create(Entity newTreeEntity) const {
    const auto* shadowComponent = registry_.get().try_get<ShadowEntityComponent>(newTreeEntity);
    return ShadowedElementAdapter(registry_.get(), newTreeEntity,
                                  shadowComponent ? shadowComponent->lightEntity : newTreeEntity);
  }

  std::reference_wrapper<Registry> registry_;
  Entity treeEntity_;
  Entity dataEntity_;
};

struct ComputedStyleComponent {
  ComputedStyleComponent() {}

  const PropertyRegistry& properties() const {
    assert(properties_);
    return properties_.value();
  }

  const Boxd& viewbox() const {
    assert(viewbox_.has_value());
    return viewbox_.value();
  }

  void computeProperties(Registry& registry, Entity entity) {
    if (properties_) {
      return;  // Already computed.
    }

    const auto* shadowComponent = registry.try_get<ShadowEntityComponent>(entity);
    const Entity dataEntity = shadowComponent ? shadowComponent->lightEntity : entity;

    // Apply local style.
    PropertyRegistry properties;
    if (auto* styleComponent = registry.try_get<StyleComponent>(dataEntity)) {
      properties = styleComponent->properties;
    } else {
      properties = PropertyRegistry();
    }

    // Apply style from stylesheets.
    for (auto view = registry.view<StylesheetComponent>(); auto stylesheetEntity : view) {
      auto [stylesheet] = view.get(stylesheetEntity);

      for (const css::SelectorRule& rule : stylesheet.stylesheet.rules()) {
        if (css::SelectorMatchResult match =
                rule.selector.matches(ShadowedElementAdapter(registry, entity, dataEntity));
            match) {
          for (const auto& declaration : rule.declarations) {
            properties.parseProperty(declaration, match.specificity);
          }
        }
      }
    }

    // Inherit from parent.
    if (const Entity parent = registry.get<TreeComponent>(entity).parent(); parent != entt::null) {
      auto& parentStyleComponent = registry.get_or_emplace<ComputedStyleComponent>(parent);
      parentStyleComponent.computeProperties(registry, parent);

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
  std::optional<PropertyRegistry> properties_;
  std::optional<Boxd> viewbox_;
};

}  // namespace donner::svg
