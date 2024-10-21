#pragma once
/// @file

#include <map>

#include "donner/base/EcsRegistry.h"
#include "donner/base/RcString.h"
#include "donner/base/SmallVector.h"
#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/base/xml/components/AttributesComponent.h"
#include "donner/base/xml/components/TreeComponent.h"

namespace donner::xml::components {

/**
 * Manages XML namespace state for a document.
 *
 * Access via the \c Registry::ctx API:
 * ```
 * XMLNamespaceContext& context = registry.ctx().get<XMLNamespaceContext>();
 * ```
 */
class XMLNamespaceContext {
public:
  /**
   * Constructor, this should only be called once to construct on the given \ref Registry, with
   * `registry.ctx().emplace<XMLNamespaceContext>(registry)`.
   *
   * To use this class, access it via the \c Registry::ctx API.
   * ```
   * XMLNamespaceContext& context = registry.ctx().get<XMLNamespaceContext>();
   * ```
   *
   * @param registry Underlying registry for the document.
   */
  explicit XMLNamespaceContext(Registry& registry) {
    registry.on_destroy<Entity>().connect<&XMLNamespaceContext::onEntityDestroy>(*this);
  }

  /**
   * Add a namespace override for the given entity. If the attribute has changed this may be called
   * again with the same name but different URI.
   *
   * @param entity Entity to add the namespace override for.
   * @param prefix Namespace prefix to override.
   * @param uri Namespace URI to use for the prefix.
   */
  void addNamespaceOverride(Entity entity, const XMLQualifiedName& name, const RcString& uri) {
    // Map the name to a prefix (either xmlns for prefix="" or xmlns:prefix)
    RcString prefix;
    if (name.namespacePrefix.empty() && name.name == "xmlns") {
      // Default namespace declaration
      prefix = "";
    } else if (name.namespacePrefix == "xmlns") {
      // Namespace declaration with prefix
      prefix = name.name;
    } else {
      assert(false && "Not a namespace declaration attribute");
      return;
    }

    // Remove existing entries with this entity and prefix
    auto range = namespaceEntries_.equal_range(prefix);
    for (auto it = range.first; it != range.second;) {
      if (it->second.entity == entity) {
        it = namespaceEntries_.erase(it);
      } else {
        ++it;
      }
    }

    namespaceEntries_.emplace(prefix, NamespaceEntry{entity, uri});
  }

  /**
   * Removes a namespace override for the given entity.
   *
   * @param entity Entity to remove the namespace override for.
   * @param prefix Namespace prefix to remove.
   */
  void removeNamespaceOverride(Entity entity, const XMLQualifiedName& name) {
    // Map the name to a prefix (either xmlns by itself, or xmlns:prefix)
    RcString prefix;
    if (name.namespacePrefix.empty() && name.name == "xmlns") {
      // Default namespace declaration
      prefix = "";
    } else if (name.namespacePrefix == "xmlns") {
      // Namespace declaration with prefix
      prefix = name.name;
    } else {
      assert(false && "Not a namespace declaration attribute");
      return;
    }

    // Remove existing entries with this entity and prefix
    auto range = namespaceEntries_.equal_range(prefix);
    for (auto it = range.first; it != range.second;) {
      if (it->second.entity == entity) {
        it = namespaceEntries_.erase(it);
      } else {
        ++it;
      }
    }
  }

  /**
   * Get the URI for the given namespace prefix.
   *
   * @param registry Registry to use for the lookup.
   * @param entity Entity to get the namespace URI for.
   * @param prefix Namespace prefix to get the URI for.
   * @return The URI for the given namespace prefix, or an empty string if the prefix is not found.
   */
  std::optional<RcString> getNamespaceUri(Registry& registry, Entity entity,
                                          const RcString& prefix) const {
    // Get a list of parents for the entity.
    SmallVector<Entity, 8> parents = getParents(registry, entity);

    // Find entries in the multimap with the given prefix
    auto range = namespaceEntries_.equal_range(prefix);
    if (range.first == namespaceEntries_.end()) {
      return std::nullopt;
    }

    // Search for the prefix in the parents (from nearest to furthest ancestor)
    for (Entity parent : parents) {
      if (const auto* attributes =
              registry.try_get<donner::components::AttributesComponent>(parent);
          attributes && attributes->hasNamespaceOverrides()) {
        // Check for namespace entries with the given prefix and parent entity
        for (auto it = range.first; it != range.second; ++it) {
          if (it->second.entity == parent) {
            return it->second.uri;
          }
        }
      }
    }

    return std::nullopt;
  }

private:
  SmallVector<Entity, 8> getParents(Registry& registry, Entity entity) const {
    SmallVector<Entity, 8> result;
    while (entity != entt::null) {
      result.push_back(entity);
      if (const auto* tree = registry.try_get<donner::components::TreeComponent>(entity)) {
        entity = tree->parent();
      } else {
        entity = entt::null;
      }
    }
    return result;
  }

  /// Called when an entity is destroyed.
  void onEntityDestroy(Registry& registry, Entity entity) {
    if (const auto* attributes = registry.try_get<donner::components::AttributesComponent>(entity);
        attributes && attributes->hasNamespaceOverrides()) {
      // Remove all entries with this entity.
      for (auto it = namespaceEntries_.begin(); it != namespaceEntries_.end();) {
        if (it->second.entity == entity) {
          it = namespaceEntries_.erase(it);
        } else {
          ++it;
        }
      }
    }
  }

  struct NamespaceEntry {
    Entity entity;
    RcString uri;
  };

  /// Mapping from ID to entity.
  std::multimap<RcString, NamespaceEntry> namespaceEntries_;
};

}  // namespace donner::xml::components
