#include "donner/svg/compositor/ScopedCompositorHint.h"

#include <utility>

namespace donner::svg::compositor {

ScopedCompositorHint::ScopedCompositorHint(Registry& registry, Entity entity, HintSource source,
                                           uint16_t weight)
    : registry_(&registry), entity_(entity), source_(source), weight_(weight) {
  auto& component =
      registry_->get_or_emplace<CompositorHintComponent>(entity_);
  component.addHint(source_, weight_);
}

ScopedCompositorHint::~ScopedCompositorHint() {
  if (registry_ == nullptr) {
    // Moved-from; inert.
    return;
  }
  if (!registry_->valid(entity_)) {
    // Entity destroyed before us; nothing to do.
    return;
  }
  auto* component = registry_->try_get<CompositorHintComponent>(entity_);
  if (component == nullptr) {
    return;
  }
  component->removeFirstMatching(source_, weight_);
  if (component->empty()) {
    registry_->remove<CompositorHintComponent>(entity_);
  }
}

ScopedCompositorHint::ScopedCompositorHint(ScopedCompositorHint&& other) noexcept
    : registry_(other.registry_),
      entity_(other.entity_),
      source_(other.source_),
      weight_(other.weight_) {
  other.registry_ = nullptr;
  other.entity_ = entt::null;
}

ScopedCompositorHint& ScopedCompositorHint::operator=(ScopedCompositorHint&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  // Release our current entry first.
  if (registry_ != nullptr && registry_->valid(entity_)) {
    if (auto* component = registry_->try_get<CompositorHintComponent>(entity_)) {
      component->removeFirstMatching(source_, weight_);
      if (component->empty()) {
        registry_->remove<CompositorHintComponent>(entity_);
      }
    }
  }
  registry_ = other.registry_;
  entity_ = other.entity_;
  source_ = other.source_;
  weight_ = other.weight_;
  other.registry_ = nullptr;
  other.entity_ = entt::null;
  return *this;
}

ScopedCompositorHint ScopedCompositorHint::Mandatory(Registry& registry, Entity entity) {
  return ScopedCompositorHint(registry, entity, HintSource::Mandatory, 0xFFFF);
}

ScopedCompositorHint ScopedCompositorHint::Explicit(Registry& registry, Entity entity,
                                                    uint16_t weight) {
  return ScopedCompositorHint(registry, entity, HintSource::Explicit, weight);
}

}  // namespace donner::svg::compositor
