#pragma once
/// @file

#include <entt/entity/fwd.hpp>

namespace donner {

/// Entity type for the Registry.
using Entity = entt::entity;

/// Forward declaration of Registry.
using Registry = entt::basic_registry<Entity, std::allocator<Entity>>;

/// Forward declaration of EntityHandle.
using EntityHandle = entt::basic_handle<Registry>;

}  // namespace donner
