#pragma once
/// @file

#include <entt/entt.hpp>

namespace donner::svg {

/**
 * Entity type for the SVG \ref Registry, a \c std::uint32_t alias.
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

/// Ostream output operator, outputs `#<id>` where `<id>` is the numeric value of the \ref Entity.
inline std::ostream& operator<<(std::ostream& os, const Entity& entity) {
  return os << "#" << static_cast<std::uint32_t>(entity);
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
 * registry.emplace<components::TreeComponent>(entity, ElementType::Unknown, "unknown");
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

}  // namespace donner::svg
