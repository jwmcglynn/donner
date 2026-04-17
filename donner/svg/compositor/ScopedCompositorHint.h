#pragma once
/// @file

#include <cstdint>
#include <optional>

#include "donner/base/EcsRegistry.h"
#include "donner/svg/compositor/CompositorHintComponent.h"

namespace donner::svg::compositor {

/**
 * Kind of interaction the editor is signalling to the compositor. See design
 * doc § Interaction hints. `Hover` is intentionally omitted in v1 — whether v2
 * adds it at all is deferred (Non-Goal 8).
 */
enum class InteractionHint : uint8_t {
  /// The entity is selected (has selection chrome). Held across frames until
  /// selection clears.
  Selection,
  /// The user is actively dragging the entity. Held from mouse-down through
  /// mouse-up plus a short idle window.
  ActiveDrag,
};

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

  /// Factory: publish an `Interaction` hint. `kind` records the interaction
  /// semantics for future introspection; the resolver only reads the weight.
  /// Default weight `0x8000` matches the Medium slot in the design-doc weight
  /// hierarchy.
  static ScopedCompositorHint Interaction(Registry& registry, Entity entity,
                                          InteractionHint kind, uint16_t weight = 0x8000);

  /// Factory: publish an `Animation` hint. Default weight `0xC000` matches the
  /// High slot in the design-doc weight hierarchy — higher than Interaction so
  /// an actively-animating entity wins over a merely-selected one under budget
  /// pressure.
  static ScopedCompositorHint Animation(Registry& registry, Entity entity,
                                        uint16_t weight = 0xC000);

  /// Returns the entity this handle targets. Returns `entt::null` if the handle has been moved from.
  [[nodiscard]] Entity entity() const { return entity_; }

  /// Returns the hint source. Undefined if the handle has been moved from.
  [[nodiscard]] HintSource source() const { return source_; }

  /// Returns the hint weight. Undefined if the handle has been moved from.
  [[nodiscard]] uint16_t weight() const { return weight_; }

  /// Returns the `InteractionHint` kind if this handle was produced by
  /// `Interaction()`; `std::nullopt` otherwise. Recorded for introspection
  /// only — the resolver ignores it.
  [[nodiscard]] std::optional<InteractionHint> interactionKind() const {
    return (source_ == HintSource::Interaction) ? std::optional<InteractionHint>(interactionKind_)
                                                : std::nullopt;
  }

  /// Returns true if this handle is still live (has not been moved from).
  [[nodiscard]] bool active() const { return registry_ != nullptr; }

private:
  ScopedCompositorHint(Registry& registry, Entity entity, HintSource source, uint16_t weight,
                       InteractionHint interactionKind);

  Registry* registry_;
  Entity entity_;
  HintSource source_;
  uint16_t weight_;
  InteractionHint interactionKind_ = InteractionHint::Selection;
};

}  // namespace donner::svg::compositor
