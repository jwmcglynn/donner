#include "donner/base/xml/components/AttributesComponent.h"

#include "donner/base/EcsRegistry.h"
#include "donner/base/xml/components/XMLNamespaceContext.h"

namespace donner::components {

SmallVector<xml::XMLQualifiedNameRef, 1> AttributesComponent::findMatchingAttributes(
    const xml::XMLQualifiedNameRef& matcher) const {
  SmallVector<xml::XMLQualifiedNameRef, 1> result;

  if (matcher.namespacePrefix == "*") {
    const xml::XMLQualifiedNameRef attributeNameOnly(matcher.name);

    for (auto it = attributes_.lower_bound(attributeNameOnly); it != attributes_.end(); ++it) {
      if (StringUtils::Equals<StringComparison::IgnoreCase>(it->first.name, matcher.name)) {
        result.push_back(it->first);
      } else {
        break;
      }
    }
  } else if (attributes_.count(matcher)) {
    result.push_back(matcher);
  }

  return result;
}

void AttributesComponent::setAttribute(Registry& registry, const xml::XMLQualifiedNameRef& name,
                                       const RcString& value) {
  xml::XMLQualifiedName nameAllocated(RcString(name.namespacePrefix), RcString(name.name));

  auto [xmlAttrStorageIt, _inserted] = attrNameStorage_.insert(nameAllocated);
  const xml::XMLQualifiedNameRef attrRef = *xmlAttrStorageIt;

  auto [attrIt, newAttrInserted] = attributes_.emplace(attrRef, Storage(nameAllocated, value));
  if (!newAttrInserted) {
    attrIt->second.value = value;
  } else {
    if (isNamespaceOverride(nameAllocated)) {
      ++numNamespaceOverrides_;

      const Entity self = entt::to_entity(registry.storage<AttributesComponent>(), *this);
      registry.ctx().get<xml::components::XMLNamespaceContext>().addNamespaceOverride(
          self, nameAllocated, value);
    }
  }
}

void AttributesComponent::removeAttribute(Registry& registry,
                                          const xml::XMLQualifiedNameRef& name) {
  const auto it = attributes_.find(name);
  if (it != attributes_.end()) {
    const xml::XMLQualifiedName attrToRemove = std::move(it->second.name);
    attributes_.erase(it);

    // Erase the XMLQualifiedName storage _after_ the attributes map, since the attributes map key
    // takes a reference to the data in XMLQualifiedName storage.
    attrNameStorage_.erase(attrToRemove);

    if (isNamespaceOverride(attrToRemove)) {
      assert(numNamespaceOverrides_ > 0);
      --numNamespaceOverrides_;

      const Entity self = entt::to_entity(registry.storage<AttributesComponent>(), *this);
      registry.ctx().get<xml::components::XMLNamespaceContext>().removeNamespaceOverride(
          self, attrToRemove);
    }
  }
}

}  // namespace donner::components
