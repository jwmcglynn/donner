#pragma once
/// @file

#include <cstdint>

#include "donner/base/EcsRegistry.h"
#include "donner/svg/compositor/CompositorHintComponent.h"

namespace donner::svg::compositor {

/**
 * RAII handle that adds a `HintEntry` to an entity's `CompositorHintComponent`
 * on construction and removes exactly that entry on destruction.
 *
 * The primary API for hint producers. Drop the handle and the hint disappears;
 * the resolver will demote the entity on the next frame automatically.
 *
 * If the target entity has no `CompositorHintComponent` yet, one is created.
 * When the destructor removes the last entry, the component itself is removed
 * so entity storage stays lean and the resolver's view iteration stays tight.
 *
 * Non-copyable; movable. A moved-from handle is inert — its destructor does
 * nothing. The usual RAII pattern.
 */
class ScopedCompositorHint {
public:
  /**
   * Attach a hint to `entity`. Creates `CompositorHintComponent` if absent.
   *
   * @param registry Registry owning the entity.
   * @param entity Target entity.
   * @param source Hint source enum.
   * @param weight Hint weight (use `0xFFFF` with `HintSource::Mandatory`).
   */
  ScopedCompositorHint(Registry& registry, Entity entity, HintSource source, uint16_t weight);

  /// Removes the hint entry, and drops the component entirely if it was the last entry.
  ~ScopedCompositorHint();

  ScopedCompositorHint(const ScopedCompositorHint&) = delete;
  ScopedCompositorHint& operator=(const ScopedCompositorHint&) = delete;

  ScopedCompositorHint(ScopedCompositorHint&& other) noexcept;
  ScopedCompositorHint& operator=(ScopedCompositorHint&& other) noexcept;

  /// Factory: publish a `Mandatory` hint at the infinite-weight sentinel (`0xFFFF`).
  static ScopedCompositorHint Mandatory(Registry& registry, Entity entity);

  /// Factory: publish an `Explicit` hint with caller-chosen weight (defaults to the
  /// mid-range weight `0x4000`, matching the design-doc default).
  static ScopedCompositorHint Explicit(Registry& registry, Entity entity,
                                       uint16_t weight = 0x4000);

  /// Returns the entity this handle targets. Returns `entt::null` if the handle has been moved from.
  [[nodiscard]] Entity entity() const { return entity_; }

  /// Returns the hint source. Undefined if the handle has been moved from.
  [[nodiscard]] HintSource source() const { return source_; }

  /// Returns the hint weight. Undefined if the handle has been moved from.
  [[nodiscard]] uint16_t weight() const { return weight_; }

  /// Returns true if this handle is still live (has not been moved from).
  [[nodiscard]] bool active() const { return registry_ != nullptr; }

private:
  Registry* registry_;
  Entity entity_;
  HintSource source_;
  uint16_t weight_;
};

}  // namespace donner::svg::compositor
