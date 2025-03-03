#pragma once
/// @file

#include <map>

#include "donner/base/RcStringOrRef.h"

namespace donner::xml::components {

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
      const RcStringOrRef& name) const {
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
