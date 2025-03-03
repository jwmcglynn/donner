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
    registry.on_destroy<donner::components::AttributesComponent>()
        .connect<&XMLNamespaceContext::onEntityDestroy>(*this);
  }

  /**
   * Add a namespace override for the given entity. If the attribute has changed this may be called
   * again with the same name but different URI.
   *
   * @param entity Entity to add the namespace override for.
   * @param name Namespace attribute to override.
   * @param uri Namespace URI to use for the prefix.
   */
  void addNamespaceOverride(Entity entity, const XMLQualifiedName& name, const RcString& uri) {
    // Map the name to a prefix (either xmlns for prefix="" or xmlns:prefix)
    std::optional<RcString> prefix;
    if (name.namespacePrefix.empty() && name.name == "xmlns") {
      // Default namespace declaration
      prefix = "";
    } else if (name.namespacePrefix == "xmlns") {
      // Namespace declaration with prefix
      prefix = name.name;
    }

    assert(prefix.has_value() && "Not a namespace declaration attribute");

    // Remove existing entries with this entity and prefix
    auto range = namespaceEntries_.equal_range(prefix.value());
    for (auto it = range.first; it != range.second;) {
      if (it->second.entity == entity) {
        it = namespaceEntries_.erase(it);
      } else {
        ++it;
      }
    }

    namespaceEntries_.emplace(prefix.value(), NamespaceEntry{entity, uri});
  }

  /**
   * Removes a namespace override for the given entity.
   *
   * @param entity Entity to remove the namespace override for.
   * @param name Namespace attribute to remove.
   */
  void removeNamespaceOverride(Entity entity, const XMLQualifiedName& name) {
    // Map the name to a prefix (either xmlns by itself, or xmlns:prefix)
    std::optional<RcString> prefix;
    if (name.namespacePrefix.empty() && name.name == "xmlns") {
      // Default namespace declaration
      prefix = "";
    } else if (name.namespacePrefix == "xmlns") {
      // Namespace declaration with prefix
      prefix = name.name;
    }

    assert(prefix.has_value() && "Not a namespace declaration attribute");

    // Remove existing entries with this entity and prefix
    auto range = namespaceEntries_.equal_range(prefix.value());
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
    const auto& attributes = registry.get<donner::components::AttributesComponent>(entity);
    if (attributes.hasNamespaceOverrides()) {
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

  /// Entry storing the entity and URI for a given namespace override.
  struct NamespaceEntry {
    Entity entity;  ///< Entity that has the namespace override.
    RcString uri;   ///< URI for the namespace.
  };

  /// Mapping from ID to entity.
  std::multimap<RcString, NamespaceEntry> namespaceEntries_;
};

/**
 * Manages XML entity declarations for a document.
 *
 * Stores general and parameter entity declarations from DOCTYPE sections.
 * Currently used to store entity declarations, but complete entity expansion
 * in the parsed XML content is not yet implemented (TODO).
 *
 * Access via the \c Registry::ctx API:
 * ```
 * EntityDeclarationsContext& context = registry.ctx().get<EntityDeclarationsContext>();
 * ```
 */
class EntityDeclarationsContext {
public:
  /**
   * Constructor, should only be called once to construct on the given \ref Registry, with
   * `registry.ctx().emplace<EntityDeclarationsContext>()`.
   *
   * To use this class, access it via the \c Registry::ctx API.
   * ```
   * EntityDeclarationsContext& context = registry.ctx().get<EntityDeclarationsContext>();
   * ```
   */
  EntityDeclarationsContext() = default;

  /**
   * Adds a general entity declaration.
   *
   * @param name The name of the entity.
   * @param value The replacement text for the entity.
   * @param isExternal Whether this is an external entity.
   */
  void addEntityDeclaration(const RcStringOrRef& name, const RcString& value,
                            bool isExternal = false) {
    RcString nameAllocated = name;
    entityDeclarations_[nameAllocated] = {value, isExternal};
  }

  /**
   * Adds a parameter entity declaration (used within DTDs).
   *
   * @param name The name of the parameter entity.
   * @param value The replacement text for the entity.
   * @param isExternal Whether this is an external entity.
   */
  void addParameterEntityDeclaration(const RcStringOrRef& name, const RcString& value,
                                     bool isExternal = false) {
    RcString nameAllocated = name;
    parameterEntityDeclarations_[RcString(nameAllocated)] = {value, isExternal};
  }

  /**
   * Gets the replacement text for a general entity by name.
   *
   * @param name The name of the entity.
   * @return The replacement text and whether it's external, or std::nullopt if not found.
   */
  std::optional<std::pair<RcString, bool>> getEntityDeclaration(const RcStringOrRef& name) const {
    auto it = entityDeclarations_.find(name);
    if (it != entityDeclarations_.end()) {
      return std::make_pair(it->second.value, it->second.isExternal);
    }
    return std::nullopt;
  }

  /**
   * Gets the replacement text for a parameter entity by name.
   *
   * @param name The name of the parameter entity.
   * @return The replacement text and whether it's external, or std::nullopt if not found.
   */
  std::optional<std::pair<RcString, bool>> getParameterEntityDeclaration(
      const RcString& name) const {
    auto it = parameterEntityDeclarations_.find(name);
    if (it != parameterEntityDeclarations_.end()) {
      return std::make_pair(it->second.value, it->second.isExternal);
    }
    return std::nullopt;
  }

private:
  /// Information about an entity declaration
  struct EntityDeclarationInfo {
    RcString value;   ///< The replacement text or external identifier
    bool isExternal;  ///< Whether this is an external entity
  };

  /// Mapping from entity name to its declaration
  std::map<RcStringOrRef, EntityDeclarationInfo> entityDeclarations_;

  /// Mapping from parameter entity name to its declaration
  std::map<RcStringOrRef, EntityDeclarationInfo> parameterEntityDeclarations_;
};

}  // namespace donner::xml::components
