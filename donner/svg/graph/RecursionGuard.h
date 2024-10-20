#pragma once
/// @file

#include <cassert>
#include <set>

#include "donner/base/EcsRegistry.h"

namespace donner::svg {

/**
 * Helper to guard against recursion when evaluating references.
 *
 * This is used to prevent infinite recursion when reference hierarchies contain cycles.
 *
 * Usage:
 * ```
 * RecursionGuard guard;
 * Entity current = ...;
 * while (current) {
 *   Entity target = getTarget(current);
 *   if (guard.hasRecursion(target)) {
 *     break;
 *   }
 *
 *   guard.add(target);
 *   current = target;
 * }
 * ```
 *
 * There is also a shorthand when passing a \ref RecursionGuard as a parameter:
 * ```
 * void doSomething(Entity element, RecursionGuard guard) {
 *   Entity next = getNext(current);
 *   if (next != entt::null) {
 *     doSomething(next, guard.with(next));
 *   }
 * }
 * ```
 */
struct RecursionGuard {
  /// efault constructor.
  RecursionGuard() = default;

  /**
   * Returns true if this entity has been seen before, indicating a cycle has been detected.
   *
   * @param entity Entity to check.
   */
  bool hasRecursion(Entity entity) const { return (entities_.find(entity) != entities_.end()); }

  /**
   * Add the given entity to the set of entities that have been seen.
   *
   * @param entity Entity to add.
   */
  void add(Entity entity) {
    [[maybe_unused]] const auto result = entities_.insert(entity);
    assert(result.second && "New element must be inserted");
  }

  /**
   * Create a new \ref RecursionGuard with the given entity added to the set of entities that have
   * been seen.
   *
   * @param entity Entity to add.
   */
  RecursionGuard with(Entity entity) const {
    RecursionGuard result = *this;
    result.add(entity);
    return result;
  }

private:
  /// Set of entities that have been seen.
  std::set<Entity> entities_;
};

}  // namespace donner::svg
