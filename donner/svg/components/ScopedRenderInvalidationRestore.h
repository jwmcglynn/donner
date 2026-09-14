#pragma once
/// @file

#include <optional>
#include <utility>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/svg/components/DirtyFlagsComponent.h"

namespace donner::svg::components {

/// Auxiliary preparation may compute geometry, but cannot consume the canvas renderer's dirty
/// obligations. Hold under the document write guard; restore only identities still alive.
class ScopedRenderInvalidationRestore {
public:
  explicit ScopedRenderInvalidationRestore(Registry& registry) : registry_(registry) {
    if (const auto* state = registry_.ctx().find<RenderTreeState>()) state_ = *state;
    for (const Entity entity : registry_.view<DirtyFlagsComponent>()) {
      dirty_.emplace_back(entity, registry_.get<DirtyFlagsComponent>(entity).flags);
    }
  }

  ~ScopedRenderInvalidationRestore() {
    registry_.clear<DirtyFlagsComponent>();
    for (const auto& [entity, flags] : dirty_) {
      if (registry_.valid(entity)) {
        registry_.emplace_or_replace<DirtyFlagsComponent>(entity).flags = flags;
      }
    }
    registry_.ctx().erase<RenderTreeState>();
    if (state_) registry_.ctx().emplace<RenderTreeState>(*state_);
  }

  ScopedRenderInvalidationRestore(const ScopedRenderInvalidationRestore&) = delete;
  ScopedRenderInvalidationRestore& operator=(const ScopedRenderInvalidationRestore&) = delete;

private:
  Registry& registry_;
  std::optional<RenderTreeState> state_;
  std::vector<std::pair<Entity, uint16_t>> dirty_;
};

}  // namespace donner::svg::components
