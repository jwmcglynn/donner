#pragma once
/// @file

#include <map>

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
