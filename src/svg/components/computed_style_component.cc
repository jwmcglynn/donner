#include "src/svg/components/computed_style_component.h"

#include "src/svg/components/class_component.h"
#include "src/svg/components/document_context.h"
#include "src/svg/components/shadow_entity_component.h"
#include "src/svg/components/sized_element_component.h"
#include "src/svg/components/style_component.h"
#include "src/svg/components/stylesheet_component.h"
#include "src/svg/components/tree_component.h"
#include "src/svg/components/viewbox_component.h"

namespace donner::svg {

namespace {

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

}  // namespace

void ComputedStyleComponent::computeProperties(EntityHandle handle) {
  if (properties_) {
    return;  // Already computed.
  }

  Registry& registry = *handle.registry();

  const auto* shadowComponent = handle.try_get<ShadowEntityComponent>();
  const Entity dataEntity = shadowComponent ? shadowComponent->lightEntity : handle.entity();

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
              rule.selector.matches(ShadowedElementAdapter(registry, handle.entity(), dataEntity));
          match) {
        for (const auto& declaration : rule.declarations) {
          properties.parseProperty(declaration, match.specificity);
        }
      }
    }
  }

  // Inherit from parent.
  if (const Entity parent = handle.get<TreeComponent>().parent(); parent != entt::null) {
    auto& parentStyleComponent = registry.get_or_emplace<ComputedStyleComponent>(parent);
    parentStyleComponent.computeProperties(EntityHandle(registry, parent));

    // <pattern> elements can't inherit 'fill' or 'stroke' or it creates recursion in the shadow
    // tree.
    const PropertyInheritOptions inheritOptions =
        registry.all_of<DoNotInheritFillOrStrokeTag>(parent) ? PropertyInheritOptions::NoPaint
                                                             : PropertyInheritOptions::All;

    properties_ = properties.inheritFrom(parentStyleComponent.properties(), inheritOptions);
    viewbox_ = parentStyleComponent.viewbox_;
  } else {
    properties_ = properties;
  }

  // If we don't have a parent, inherit the one from the root entity (which may also be this
  // entity).
  if (!viewbox_) {
    const Entity rootEntity = registry.ctx<DocumentContext>().rootEntity;
    const Vector2i documentSize =
        registry.get<SizedElementComponent>(rootEntity)
            .calculateViewportScaledDocumentSize(registry, InvalidSizeBehavior::ZeroSize);
    viewbox_ = Boxd(Vector2d::Zero(), documentSize);
  }

  // If the viewbox is not set at this point, this is an error, since it means that we didn't
  // traverse through an SVG element.
  assert(viewbox_.has_value());

  // Note that this may override the viewbox specified if we're the rootEntity above.
  if (auto* viewboxComponent = handle.try_get<ViewboxComponent>()) {
    // If there's a viewbox, we need to resolve units with the parent viewbox.
    properties_->resolveUnits(viewbox_.value(), FontMetrics());

    if (viewboxComponent->viewbox) {
      viewbox_ = viewboxComponent->viewbox.value();
    } else {
      // TODO: This is a strange dependency inversion, where ComputedStyleComponent depends on
      // SizedElementComponent which depends on ComputedStyleComponent to calculate the viewbox.
      // Split the computed viewbox into a different component?
      // TODO: Pass outWarnings?
      handle.get<SizedElementComponent>().computeWithPrecomputedStyle(handle, *this, FontMetrics(),
                                                                      nullptr);
      viewbox_ = handle.get<ComputedSizedElementComponent>().bounds;
    }
  } else {
    // Convert properties to relative transforms.
    // TODO: Set font metrics from properties.
    properties_->resolveUnits(viewbox_.value(), FontMetrics());
  }
}

}  // namespace donner::svg