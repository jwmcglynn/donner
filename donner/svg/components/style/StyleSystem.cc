#include "donner/svg/components/style/StyleSystem.h"

#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/svg/components/AttributesComponent.h"
#include "donner/svg/components/ClassComponent.h"
#include "donner/svg/components/IdComponent.h"
#include "donner/svg/components/StylesheetComponent.h"
#include "donner/svg/components/TreeComponent.h"
#include "donner/svg/components/shadow/ShadowEntityComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/style/DoNotInheritFillOrStrokeTag.h"
#include "donner/svg/components/style/StyleComponent.h"
#include "donner/svg/registry/Registry.h"

namespace donner::svg::components {

namespace {

struct ShadowedElementAdapter {
  ShadowedElementAdapter(Registry& registry, Entity treeEntity, Entity dataEntity)
      : registry_(registry), treeEntity_(treeEntity), dataEntity_(dataEntity) {}

  bool operator==(const ShadowedElementAdapter& other) const {
    return treeEntity_ == other.treeEntity_;
  }

  Entity entity() const { return treeEntity_; }

  std::optional<ShadowedElementAdapter> parentElement() const {
    const Entity target = registry_.get().get<TreeComponent>(treeEntity_).parent();
    return target != entt::null ? std::make_optional(create(target)) : std::nullopt;
  }

  std::optional<ShadowedElementAdapter> firstChild() const {
    const Entity target = registry_.get().get<TreeComponent>(treeEntity_).firstChild();
    return target != entt::null ? std::make_optional(create(target)) : std::nullopt;
  }

  std::optional<ShadowedElementAdapter> lastChild() const {
    const Entity target = registry_.get().get<TreeComponent>(treeEntity_).lastChild();
    return target != entt::null ? std::make_optional(create(target)) : std::nullopt;
  }

  std::optional<ShadowedElementAdapter> previousSibling() const {
    const Entity target = registry_.get().get<TreeComponent>(treeEntity_).previousSibling();
    return target != entt::null ? std::make_optional(create(target)) : std::nullopt;
  }

  std::optional<ShadowedElementAdapter> nextSibling() const {
    const Entity target = registry_.get().get<TreeComponent>(treeEntity_).nextSibling();
    return target != entt::null ? std::make_optional(create(target)) : std::nullopt;
  }

  XMLQualifiedNameRef tagName() const {
    return registry_.get().get<TreeComponent>(treeEntity_).tagName();
  }

  bool isKnownType() const {
    return registry_.get().get<TreeComponent>(treeEntity_).type() != svg::ElementType::Unknown;
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

  bool hasAttribute(const XMLQualifiedNameRef& name) const {
    if (const auto* component = registry_.get().try_get<AttributesComponent>(dataEntity_)) {
      return component->hasAttribute(name);
    } else {
      return false;
    }
  }

  std::optional<RcString> getAttribute(const XMLQualifiedNameRef& name) const {
    if (const auto* component = registry_.get().try_get<AttributesComponent>(dataEntity_)) {
      return component->getAttribute(name);
    } else {
      return std::nullopt;
    }
  }

  SmallVector<XMLQualifiedNameRef, 1> findMatchingAttributes(
      const XMLQualifiedNameRef& matcher) const {
    if (const auto* component = registry_.get().try_get<AttributesComponent>(dataEntity_)) {
      return component->findMatchingAttributes(matcher);
    } else {
      return {};
    }
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

const ComputedStyleComponent& StyleSystem::computeStyle(
    EntityHandle handle, std::vector<parser::ParseError>* outWarnings) {
  auto& computedStyle = handle.get_or_emplace<ComputedStyleComponent>();
  computePropertiesInto(handle, computedStyle, outWarnings);
  return computedStyle;
}

void StyleSystem::computePropertiesInto(EntityHandle handle, ComputedStyleComponent& computedStyle,
                                        std::vector<parser::ParseError>* outWarnings) {
  if (computedStyle.properties) {
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
          if (auto error = properties.parseProperty(declaration, match.specificity)) {
            if (outWarnings) {
              outWarnings->push_back(std::move(error.value()));
            }
          }
        }
      }
    }
  }

  // Inherit from parent.
  if (const Entity parent = handle.get<TreeComponent>().parent(); parent != entt::null) {
    auto& parentStyleComponent = registry.get_or_emplace<ComputedStyleComponent>(parent);
    computePropertiesInto(EntityHandle(registry, parent), parentStyleComponent, outWarnings);

    // <pattern> elements can't inherit 'fill' or 'stroke' or it creates recursion in the shadow
    // tree.
    const PropertyInheritOptions inheritOptions =
        registry.all_of<DoNotInheritFillOrStrokeTag>(parent) ? PropertyInheritOptions::NoPaint
                                                             : PropertyInheritOptions::All;

    computedStyle.properties =
        properties.inheritFrom(parentStyleComponent.properties.value(), inheritOptions);
  } else {
    computedStyle.properties = properties;
  }
}

void StyleSystem::computeAllStyles(Registry& registry,
                                   std::vector<parser::ParseError>* outWarnings) {
  // Create placeholder ComputedStyleComponents for all elements in the range, since creating
  // computed style components also creates the parents, and we can't modify the component list
  // while iterating it.
  auto view = registry.view<TreeComponent>();
  for (auto entity : view) {
    std::ignore = registry.get_or_emplace<ComputedStyleComponent>(entity);
  }

  // Compute the styles for all elements.
  for (auto entity : view) {
    StyleSystem().computeStyle(EntityHandle(registry, entity), outWarnings);
  }
}

void StyleSystem::computeStylesFor(Registry& registry, std::span<const Entity> entities,
                                   std::vector<parser::ParseError>* outWarnings) {
  for (Entity entity : entities) {
    StyleSystem().computeStyle(EntityHandle(registry, entity), outWarnings);
  }
}

}  // namespace donner::svg::components
