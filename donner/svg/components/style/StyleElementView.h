#pragma once
/// @file
/// How the cascade reads an element for selector keys and ancestors: a shadow entity takes its tag
/// from its own tree entity and its id, classes and attributes from the light element it clones.

#include <string_view>

#include "donner/base/EcsRegistry.h"
#include "donner/base/StringUtils.h"
#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/base/xml/components/AttributesComponent.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/ClassComponent.h"
#include "donner/svg/components/ElementTypeComponent.h"
#include "donner/svg/components/IdComponent.h"
#include "donner/svg/components/shadow/ShadowEntityComponent.h"

namespace donner::svg::components {

/// The entity that holds \p treeEntity's id, classes and attributes: the light element a shadow
/// entity clones, or \p treeEntity itself.
inline Entity StyleDataEntity(const Registry& registry, Entity treeEntity) {
  const auto* shadow = registry.try_get<ShadowEntityComponent>(treeEntity);
  return shadow != nullptr ? shadow->lightEntity : treeEntity;
}

/**
 * The parent of \p treeEntity as selector matching walks to it, or `entt::null` where the walk
 * stops: at a parent that is not an SVG element. Shadow entities carry no
 * \ref ElementTypeComponent themselves, so a parent is checked through its data entity; otherwise
 * a shadow entity whose parent is also a shadow entity would appear parentless. A `<use>` main
 * shadow root is a tree child of its host, so a clone's walk continues into the host and its
 * ancestors.
 */
inline Entity StyleParentElement(const Registry& registry, Entity treeEntity) {
  const Entity parent = registry.get<donner::components::TreeComponent>(treeEntity).parent();
  if (parent == entt::null ||
      !registry.all_of<ElementTypeComponent>(StyleDataEntity(registry, parent))) {
    return entt::null;
  }
  return parent;
}

/// An element as the rule index and the ancestor filter read its keys: straight from its
/// components, without copies. A shadow entity reads its tag from its own tree entity and its id,
/// classes and attributes from its light entity.
class RuleIndexElementView {
public:
  /**
   * @param registry Registry holding the element.
   * @param treeEntity The element's tree entity.
   * @param dataEntity Its data entity, see \ref StyleDataEntity.
   */
  RuleIndexElementView(const Registry& registry, Entity treeEntity, Entity dataEntity,
                       bool* tagOnly = nullptr)
      : registry_(registry), treeEntity_(treeEntity), dataEntity_(dataEntity), tagOnly_(tagOnly) {}

  /// The element's id, or empty.
  std::string_view id() const {
    observeOtherKey();
    const auto* component = registry_.try_get<IdComponent>(dataEntity_);
    return component != nullptr ? std::string_view(component->id()) : std::string_view();
  }

  /// The local tag name, in any case.
  std::string_view typeName() const {
    return registry_.get<donner::components::TreeComponent>(treeEntity_).tagName().name;
  }

  /// Raw class text, for callers that must bound tokenization before starting.
  std::string_view className() const {
    const auto* component = registry_.try_get<ClassComponent>(dataEntity_);
    return component ? std::string_view(component->className) : std::string_view();
  }

  /// Number of attributes, without visiting their names.
  std::size_t attributeCount() const {
    const auto* component = registry_.try_get<donner::components::AttributesComponent>(dataEntity_);
    return component ? component->attributeCount() : 0;
  }

  /// Calls \p visit with each token of the `class` list.
  template <typename Visitor>
  void forEachClassName(Visitor&& visit) const {
    observeOtherKey();
    if (const auto* component = registry_.try_get<ClassComponent>(dataEntity_)) {
      for (std::string_view name : StringUtils::SplitAsciiWhitespace(component->className)) {
        visit(name);
      }
    }
  }

  /// Calls \p visit with the local name of each attribute, whatever its namespace.
  template <typename Visitor>
  void forEachAttributeLocalName(Visitor&& visit) const {
    observeOtherKey();
    if (const auto* component =
            registry_.try_get<donner::components::AttributesComponent>(dataEntity_)) {
      component->forEachAttributeName(
          [&](const xml::XMLQualifiedNameRef& name) { visit(std::string_view(name.name)); });
    }
  }

private:
  void observeOtherKey() const {
    if (tagOnly_) {
      *tagOnly_ = false;
    }
  }
  const Registry& registry_;
  Entity treeEntity_;
  Entity dataEntity_;
  bool* tagOnly_;
};

}  // namespace donner::svg::components
