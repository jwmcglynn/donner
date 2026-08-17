#pragma once
/// @file

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <type_traits>
#include <unordered_map>

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
    const std::optional<RcString> prefix = ToNamespacePrefix(name);
    assert(prefix.has_value() && "Not a namespace declaration attribute");
    if (!prefix.has_value()) {
      return;
    }

    declarations_[entity].insert_or_assign(prefix.value(), uri);
    invalidateScopes();
  }

  /**
   * Removes a namespace override for the given entity.
   *
   * @param entity Entity to remove the namespace override for.
   * @param name Namespace attribute to remove.
   */
  void removeNamespaceOverride(Entity entity, const XMLQualifiedName& name) {
    const std::optional<RcString> prefix = ToNamespacePrefix(name);
    assert(prefix.has_value() && "Not a namespace declaration attribute");
    if (!prefix.has_value()) {
      return;
    }

    const auto it = declarations_.find(entity);
    if (it != declarations_.end()) {
      it->second.erase(prefix.value());
      if (it->second.empty()) {
        declarations_.erase(it);
      }
    }

    invalidateScopes();
  }

  /**
   * Get the URI for the given namespace prefix.
   *
   * Resolved scopes are cached, so the first lookup below an element pays for the ancestor walk
   * and later ones are a map lookup. Like the rest of the document's read paths that populate
   * state on demand, this must not run concurrently with another access to the same document.
   *
   * @param registry Registry to use for the lookup.
   * @param entity Entity to get the namespace URI for.
   * @param prefix Namespace prefix to get the URI for.
   * @return The URI for the given namespace prefix, or an empty string if the prefix is not found.
   */
  std::optional<RcString> getNamespaceUri(Registry& registry, Entity entity,
                                          const RcString& prefix) const {
    const ScopeMap& scope = *resolveScope(registry, entity);

    const auto it = scope.find(prefix);
    if (it == scope.end()) {
      return std::nullopt;
    }

    return it->second;
  }

  /**
   * Drop cached namespace scopes for a subtree that was just attached or detached.
   *
   * An element's in-scope namespaces come from its ancestors, so re-parenting a node can change
   * which declarations apply to it and to everything below it, and nothing else. Registries
   * without an \ref XMLNamespaceContext are ignored.
   *
   * @param registry Registry whose tree changed.
   * @param entity Root of the subtree that moved.
   */
  static void InvalidateScopesForTreeChange(Registry& registry, Entity entity) {
    if (registry.ctx().contains<XMLNamespaceContext>()) {
      registry.ctx().get<XMLNamespaceContext>().invalidateSubtreeScopes(registry, entity);
    }
  }

  /// Number of entities whose scope has been computed from their ancestors, which only happens
  /// when the entity has no cached scope. Lets a test tell a cache hit from a recomputation.
  [[nodiscard]] std::uint64_t scopeResolutionCountForTesting() const {
    return scopeResolutionCount_;
  }

private:
  /// Namespace declarations made by a single element, mapping prefix to URI. The default
  /// namespace (`xmlns="..."`) uses an empty prefix.
  using DeclarationMap = std::map<RcString, RcString>;

  /// Every prefix in scope for an element: its own declarations layered over its ancestors'.
  using ScopeMap = DeclarationMap;

  /// Hash for an \c Entity key, which is a scoped enum over an integer id.
  struct EntityHash {
    std::size_t operator()(Entity entity) const {
      return std::hash<std::underlying_type_t<Entity>>{}(
          static_cast<std::underlying_type_t<Entity>>(entity));
    }
  };

  /// Map the `xmlns` or `xmlns:prefix` attribute name to the prefix it declares, or
  /// `std::nullopt` if the name is not a namespace declaration.
  static std::optional<RcString> ToNamespacePrefix(const XMLQualifiedName& name) {
    if (name.namespacePrefix.empty() && name.name == "xmlns") {
      // Default namespace declaration
      return RcString("");
    } else if (name.namespacePrefix == "xmlns") {
      // Namespace declaration with prefix
      return name.name;
    }

    return std::nullopt;
  }

  /// The scope used by elements with no namespace declarations anywhere above them.
  static const std::shared_ptr<const ScopeMap>& EmptyScope() {
    static const std::shared_ptr<const ScopeMap> kEmpty = std::make_shared<const ScopeMap>();
    return kEmpty;
  }

  /**
   * Return every prefix in scope for \p entity, computing and caching it if needed.
   *
   * Resolving walks up to the nearest ancestor that already has a cached scope and then extends
   * it downwards, so a document costs one ancestor walk in total rather than one per lookup.
   *
   * @param registry Registry to use for the lookup.
   * @param entity Entity to resolve the scope for.
   */
  const std::shared_ptr<const ScopeMap>& resolveScope(Registry& registry, Entity entity) const {
    if (entity == entt::null) {
      return EmptyScope();
    }

    // Walk up until we reach a cached scope or run out of ancestors.
    SmallVector<Entity, 8> pending;
    const std::shared_ptr<const ScopeMap>* nearestCached = &EmptyScope();

    Entity current = entity;
    while (current != entt::null) {
      const auto it = scopeCache_.find(current);
      if (it != scopeCache_.end()) {
        nearestCached = &it->second;
        break;
      }

      pending.push_back(current);
      if (const auto* tree = registry.try_get<donner::components::TreeComponent>(current)) {
        current = tree->parent();
      } else {
        current = entt::null;
      }
    }

    // Extend that scope back down towards `entity`, caching each level on the way.
    std::shared_ptr<const ScopeMap> scope = *nearestCached;
    for (size_t i = pending.size(); i > 0; --i) {
      const Entity pendingEntity = pending[i - 1];
      scope = extendScope(registry, pendingEntity, std::move(scope));
      scopeCache_.insert_or_assign(pendingEntity, scope);
      ++scopeResolutionCount_;
    }

    // `entity` is either the deepest entry just cached, or was already cached.
    const auto it = scopeCache_.find(entity);
    return it != scopeCache_.end() ? it->second : *nearestCached;
  }

  /**
   * Layer \p entity's own namespace declarations over \p parentScope.
   *
   * Elements that declare nothing share the parent's scope instead of copying it, so a document
   * that declares `xmlns` only on the root holds a single map.
   *
   * @param registry Registry to use for the lookup.
   * @param entity Entity whose declarations are applied.
   * @param parentScope Scope inherited from the entity's ancestors.
   */
  std::shared_ptr<const ScopeMap> extendScope(Registry& registry, Entity entity,
                                              std::shared_ptr<const ScopeMap> parentScope) const {
    const auto* attributes = registry.try_get<donner::components::AttributesComponent>(entity);
    if (attributes == nullptr || !attributes->hasNamespaceOverrides()) {
      return parentScope;
    }

    const auto it = declarations_.find(entity);
    if (it == declarations_.end() || it->second.empty()) {
      return parentScope;
    }

    auto extended = std::make_shared<ScopeMap>(*parentScope);
    for (const auto& [prefix, uri] : it->second) {
      extended->insert_or_assign(prefix, uri);
    }

    return extended;
  }

  /// Drop all cached scopes, after a declaration change that may apply anywhere below the
  /// declaring element.
  void invalidateScopes() { scopeCache_.clear(); }

  /// Drop cached scopes for \p entity and everything below it.
  void invalidateSubtreeScopes(Registry& registry, Entity entity) {
    if (scopeCache_.empty()) {
      return;
    }

    SmallVector<Entity, 16> stack;
    stack.push_back(entity);

    while (!stack.empty()) {
      const Entity current = stack.back();
      stack.pop_back();
      scopeCache_.erase(current);

      if (const auto* tree = registry.try_get<donner::components::TreeComponent>(current)) {
        for (Entity child = tree->firstChild(); child != entt::null;) {
          stack.push_back(child);
          child = registry.get<donner::components::TreeComponent>(child).nextSibling();
        }
      }
    }
  }

  /// Called when an entity is destroyed.
  void onEntityDestroy(Registry& registry, Entity entity) {
    scopeCache_.erase(entity);

    const auto& attributes = registry.get<donner::components::AttributesComponent>(entity);
    if (attributes.hasNamespaceOverrides()) {
      declarations_.erase(entity);
      invalidateScopes();
    }
  }

  /// Namespace declarations made by each element that has any.
  std::unordered_map<Entity, DeclarationMap, EntityHash> declarations_;

  /**
   * Resolved scopes, rebuilt on demand after any declaration or tree-structure change.
   *
   * Entries are keyed by entity and evicted three ways: a tree change drops the moved subtree, a
   * declaration change drops everything, and destroying an entity's AttributesComponent drops
   * that entity. The last one does not cover an ancestor that never had an AttributesComponent
   * (it is only added on first attribute access), so such an entry can outlive its entity. That
   * is safe rather than merely unlikely: removing a node from the tree already drops its
   * subtree, and entt bumps an entity's version when the id is released, so a recycled id is a
   * different key and can never hit a stale entry.
   */
  mutable std::unordered_map<Entity, std::shared_ptr<const ScopeMap>, EntityHash> scopeCache_;

  /// Counts entities whose scope was computed rather than read from \ref scopeCache_.
  mutable std::uint64_t scopeResolutionCount_ = 0;
};

}  // namespace donner::xml::components
