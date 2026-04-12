#pragma once
/// @file

#include <ostream>

#include <entt/entity/entity.hpp>    // entt::entity, entt::null
#include <entt/entity/fwd.hpp>       // Forward declarations
#include <entt/entity/handle.hpp>    // basic_handle definition
#include <entt/entity/registry.hpp>  // basic_registry definition

namespace donner {

/**
 * Entity type for the \ref Registry, a \c std::uint32_t alias.
 *
 * This is a core type for the the ECS, and is used to identify entities in the \ref Registry.
 *
 * @see \ref EcsArchitecture
 * @see \ref Registry
 */
using Entity = entt::entity;

/// Compare two \ref Entity values.
inline auto operator<=>(Entity lhs, Entity rhs) {
  return static_cast<std::uint32_t>(lhs) <=> static_cast<std::uint32_t>(rhs);
}

/**
 * Registry type for the SVG ECS, which is the entry point for storing all data.
 *
 * It is used to create new entities:
 * ```
 * Registry registry;
 * const Entity entity = registry.create();
 * ```
 *
 * Attach or remove data classes to entities:
 * ```
 * registry.emplace<components::TreeComponent>(entity, "unknown");
 * registry.remove<components::TreeComponent>(entity);
 * ```
 *
 * Store global objects (singleton-like):
 * ```
 * registry.ctx().emplace<components::RenderingContext>(registry));
 * const Entity root = registry.ctx().get<components::RenderingContext>().rootEntity;
 * ```
 *
 * @see \ref EcsArchitecture
 */
using Registry = entt::basic_registry<Entity, std::allocator<Entity>>;

/**
 * Convenience handle for a \ref Entity with an attached \ref Registry.
 *
 * Allows calling functions typically on \ref Registry without having to pass around two values.
 */
using EntityHandle = entt::basic_handle<Registry>;

}  // namespace donner

namespace entt {

/// Ostream output operator for entt::entity, outputs `#<id>`.
inline std::ostream& operator<<(std::ostream& os, const entity& e) {
  return os << "#" << static_cast<std::uint32_t>(e);
}

}  // namespace entt
