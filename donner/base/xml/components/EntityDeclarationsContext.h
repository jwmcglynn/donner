#pragma once
/// @file

#include <cassert>
#include <map>
#include <vector>

#include "donner/base/RcStringOrRef.h"

namespace donner::xml::components {

/// Determines the entity type, either prefixed by '&' or '%'.
enum class EntityType : uint8_t {
  General,   ///< General entity expansion, e.g. '&amp;'
  Parameter  ///< Parameter entity expansion, e.g. '%foo;', for use in the DTD.
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
   * Adds an entity declaration.
   *
   * @param type The type of the entity (general or parameter).
   * @param name The name of the entity.
   * @param value The replacement text for the entity.
   * @param isExternal Whether this is an external entity.
   */
  void addEntityDeclaration(EntityType type, const RcStringOrRef& name, const RcString& value,
                            bool isExternal = false) {
    RcString nameAllocated = name;
    if (type == EntityType::General) {
      entityDeclarations_[nameAllocated] = {value, isExternal};
    } else if (type == EntityType::Parameter) {
      parameterEntityDeclarations_[nameAllocated] = {value, isExternal};
    }
  }

  /**
   * Gets the replacement text for a general entity by name.
   *
   * @param type The type of the entity (general or parameter).
   * @param name The name of the entity.
   * @return The replacement text and whether it's external, or std::nullopt if not found.
   */
  std::optional<std::pair<RcString, bool>> getEntityDeclaration(EntityType type,
                                                                const RcStringOrRef& name) const {
    const auto& declarationsMap =
        type == EntityType::General ? entityDeclarations_ : parameterEntityDeclarations_;

    auto it = declarationsMap.find(name);
    if (it != declarationsMap.end()) {
      return std::make_pair(it->second.value, it->second.isExternal);
    }
    return std::nullopt;
  }

  /// Rebuild retained entity names and values during parse finalization.
  template <typename RemapFn>
  void remapStrings(RemapFn&& remap) {
    remapDeclarationMap(entityDeclarations_, remap);
    remapDeclarationMap(parameterEntityDeclarations_, remap);
  }

  /// Visit every retained entity string before parse finalization.
  template <typename VisitFn>
  void visitStrings(VisitFn&& visit) const {
    visitDeclarationMap(entityDeclarations_, visit);
    visitDeclarationMap(parameterEntityDeclarations_, visit);
  }

private:
  /// Information about an entity declaration
  struct EntityDeclarationInfo {
    RcString value;   ///< The replacement text or external identifier
    bool isExternal;  ///< Whether this is an external entity
  };

  using DeclarationMap = std::map<RcStringOrRef, EntityDeclarationInfo>;

  template <typename RemapFn>
  static void remapDeclarationMap(DeclarationMap& declarations, RemapFn&& remap) {
    std::vector<typename DeclarationMap::node_type> nodes;
    nodes.reserve(declarations.size());
    while (!declarations.empty()) {
      nodes.push_back(declarations.extract(declarations.begin()));
    }
    for (auto& node : nodes) {
      node.key() = RcStringOrRef(remap(RcString(node.key())));
      node.mapped().value = remap(node.mapped().value);
      const auto insertResult = declarations.insert(std::move(node));
      assert(insertResult.inserted);
    }
  }

  template <typename VisitFn>
  static void visitDeclarationMap(const DeclarationMap& declarations, VisitFn&& visit) {
    for (const auto& [name, info] : declarations) {
      visit(RcString(name));
      visit(info.value);
    }
  }

  /// Mapping from entity name to its declaration
  DeclarationMap entityDeclarations_;

  /// Mapping from parameter entity name to its declaration
  DeclarationMap parameterEntityDeclarations_;
};

}  // namespace donner::xml::components
